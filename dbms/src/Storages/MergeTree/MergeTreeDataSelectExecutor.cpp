#include <DB/Storages/MergeTree/MergeTreeDataSelectExecutor.h>
#include <DB/Interpreters/ExpressionAnalyzer.h>
#include <DB/Parsers/ASTIdentifier.h>
#include <DB/DataStreams/ExpressionBlockInputStream.h>
#include <DB/DataStreams/FilterBlockInputStream.h>
#include <DB/DataStreams/ConcatBlockInputStream.h>
#include <DB/DataStreams/CollapsingFinalBlockInputStream.h>
#include <DB/DataTypes/DataTypesNumberFixed.h>

namespace DB
{

MergeTreeDataSelectExecutor::MergeTreeDataSelectExecutor(MergeTreeData & data_) : data(data_), log(&Logger::get("MergeTreeDataSelectExecutor"))
{
	min_marks_for_seek = (data.settings.min_rows_for_seek + data.index_granularity - 1) / data.index_granularity;
	min_marks_for_concurrent_read = (data.settings.min_rows_for_concurrent_read + data.index_granularity - 1) / data.index_granularity;
	max_marks_to_use_cache = (data.settings.max_rows_to_use_cache + data.index_granularity - 1) / data.index_granularity;


}

BlockInputStreams MergeTreeDataSelectExecutor::read(
	const Names & column_names_to_return,
	ASTPtr query,
	const Settings & settings,
	QueryProcessingStage::Enum & processed_stage,
	size_t max_block_size,
	unsigned threads)
{
	data.check(column_names_to_return);
	processed_stage = QueryProcessingStage::FetchColumns;

	PKCondition key_condition(query, data.context, data.getColumnsList(), data.getSortDescription());
	PKCondition date_condition(query, data.context, data.getColumnsList(), SortDescription(1, SortColumnDescription(data.date_column_name, 1)));

	MergeTreeData::DataPartsVector parts;

	/// Выберем куски, в которых могут быть данные, удовлетворяющие date_condition.
	{
		MergeTreeData::DataParts data_parts = data.getDataParts();

		for (MergeTreeData::DataParts::iterator it = data_parts.begin(); it != data_parts.end(); ++it)
		{
			Field left = static_cast<UInt64>((*it)->left_date);
			Field right = static_cast<UInt64>((*it)->right_date);

			if (date_condition.mayBeTrueInRange(&left, &right))
				parts.push_back(*it);
		}
	}

	/// Семплирование.
	Names column_names_to_read = column_names_to_return;
	UInt64 sampling_column_value_limit = 0;
	typedef Poco::SharedPtr<ASTFunction> ASTFunctionPtr;
	ASTFunctionPtr filter_function;
	ExpressionActionsPtr filter_expression;

	ASTSelectQuery & select = *dynamic_cast<ASTSelectQuery*>(&*query);
	if (select.sample_size)
	{
		double size = apply_visitor(FieldVisitorConvertToNumber<double>(),
			dynamic_cast<ASTLiteral&>(*select.sample_size).value);

		if (size < 0)
			throw Exception("Negative sample size", ErrorCodes::ARGUMENT_OUT_OF_BOUND);

		if (size > 1)
		{
			size_t requested_count = apply_visitor(FieldVisitorConvertToNumber<UInt64>(), dynamic_cast<ASTLiteral&>(*select.sample_size).value);

			/// Узнаем, сколько строк мы бы прочли без семплирования.
			LOG_DEBUG(log, "Preliminary index scan with condition: " << key_condition.toString());
			size_t total_count = 0;
			for (size_t i = 0; i < parts.size(); ++i)
			{
				MergeTreeData::DataPartPtr & part = parts[i];
				MarkRanges ranges = markRangesFromPkRange(part->index, key_condition);

				for (size_t j = 0; j < ranges.size(); ++j)
					total_count += ranges[j].end - ranges[j].begin;
			}
			total_count *= data.index_granularity;

			size = std::min(1., static_cast<double>(requested_count) / total_count);

			LOG_DEBUG(log, "Selected relative sample size: " << size);
		}

		UInt64 sampling_column_max = 0;
		DataTypePtr type = data.getPrimaryExpression()->getSampleBlock().getByName(data.sampling_expression->getColumnName()).type;

		if (type->getName() == "UInt64")
			sampling_column_max = std::numeric_limits<UInt64>::max();
		else if (type->getName() == "UInt32")
			sampling_column_max = std::numeric_limits<UInt32>::max();
		else if (type->getName() == "UInt16")
			sampling_column_max = std::numeric_limits<UInt16>::max();
		else if (type->getName() == "UInt8")
			sampling_column_max = std::numeric_limits<UInt8>::max();
		else
			throw Exception("Invalid sampling column type in storage parameters: " + type->getName() + ". Must be unsigned integer type.", ErrorCodes::ILLEGAL_TYPE_OF_COLUMN_FOR_FILTER);

		/// Добавим условие, чтобы отсечь еще что-нибудь при повторном просмотре индекса.
		sampling_column_value_limit = static_cast<UInt64>(size * sampling_column_max);
		if (!key_condition.addCondition(data.sampling_expression->getColumnName(),
			Range::createRightBounded(sampling_column_value_limit, true)))
			throw Exception("Sampling column not in primary key", ErrorCodes::ILLEGAL_COLUMN);

		/// Выражение для фильтрации: sampling_expression <= sampling_column_value_limit

		ASTPtr filter_function_args = new ASTExpressionList;
		filter_function_args->children.push_back(data.sampling_expression);
		filter_function_args->children.push_back(new ASTLiteral(StringRange(), sampling_column_value_limit));

		filter_function = new ASTFunction;
		filter_function->name = "lessOrEquals";
		filter_function->arguments = filter_function_args;
		filter_function->children.push_back(filter_function->arguments);

		filter_expression = ExpressionAnalyzer(filter_function, data.context, data.getColumnsList()).getActions(false);

		/// Добавим столбцы, нужные для sampling_expression.
		std::vector<String> add_columns = filter_expression->getRequiredColumns();
		column_names_to_read.insert(column_names_to_read.end(), add_columns.begin(), add_columns.end());
		std::sort(column_names_to_read.begin(), column_names_to_read.end());
		column_names_to_read.erase(std::unique(column_names_to_read.begin(), column_names_to_read.end()), column_names_to_read.end());
	}

	LOG_DEBUG(log, "Key condition: " << key_condition.toString());
	LOG_DEBUG(log, "Date condition: " << date_condition.toString());

	/// PREWHERE
	ExpressionActionsPtr prewhere_actions;
	String prewhere_column;
	if (select.prewhere_expression)
	{
		ExpressionAnalyzer analyzer(select.prewhere_expression, data.context, data.getColumnsList());
		prewhere_actions = analyzer.getActions(false);
		prewhere_column = select.prewhere_expression->getColumnName();
		/// TODO: Чтобы работали подзапросы в PREWHERE, можно тут сохранить analyzer.getSetsWithSubqueries(), а потом их выполнить.
	}

	RangesInDataParts parts_with_ranges;

	/// Найдем, какой диапазон читать из каждого куска.
	size_t sum_marks = 0;
	size_t sum_ranges = 0;
	for (size_t i = 0; i < parts.size(); ++i)
	{
		MergeTreeData::DataPartPtr & part = parts[i];
		RangesInDataPart ranges(part);
		ranges.ranges = markRangesFromPkRange(part->index, key_condition);

		if (!ranges.ranges.empty())
		{
			parts_with_ranges.push_back(ranges);

			sum_ranges += ranges.ranges.size();
			for (size_t j = 0; j < ranges.ranges.size(); ++j)
			{
				sum_marks += ranges.ranges[j].end - ranges.ranges[j].begin;
			}
		}
	}

	LOG_DEBUG(log, "Selected " << parts.size() << " parts by date, " << parts_with_ranges.size() << " parts by key, "
			  << sum_marks << " marks to read from " << sum_ranges << " ranges");

	BlockInputStreams res;

	if (select.final)
	{
		/// Добавим столбцы, нужные для вычисления первичного ключа и знака.
		std::vector<String> add_columns = data.getPrimaryExpression()->getRequiredColumns();
		column_names_to_read.insert(column_names_to_read.end(), add_columns.begin(), add_columns.end());
		column_names_to_read.push_back(data.sign_column);
		std::sort(column_names_to_read.begin(), column_names_to_read.end());
		column_names_to_read.erase(std::unique(column_names_to_read.begin(), column_names_to_read.end()), column_names_to_read.end());

		res = spreadMarkRangesAmongThreadsFinal(
			parts_with_ranges,
			threads,
			column_names_to_read,
			max_block_size,
			settings.use_uncompressed_cache,
			prewhere_actions,
			prewhere_column);
	}
	else
	{
		res = spreadMarkRangesAmongThreads(
			parts_with_ranges,
			threads,
			column_names_to_read,
			max_block_size,
			settings.use_uncompressed_cache,
			prewhere_actions,
			prewhere_column);
	}

	if (select.sample_size)
	{
		for (size_t i = 0; i < res.size(); ++i)
		{
			BlockInputStreamPtr original_stream = res[i];
			BlockInputStreamPtr expression_stream = new ExpressionBlockInputStream(original_stream, filter_expression);
			BlockInputStreamPtr filter_stream = new FilterBlockInputStream(expression_stream, filter_function->getColumnName());
			res[i] = filter_stream;
		}
	}

	return res;
}

BlockInputStreams MergeTreeDataSelectExecutor::spreadMarkRangesAmongThreads(
	RangesInDataParts parts,
	size_t threads,
	const Names & column_names,
	size_t max_block_size,
	bool use_uncompressed_cache,
	ExpressionActionsPtr prewhere_actions,
	const String & prewhere_column)
{
	/// На всякий случай перемешаем куски.
	std::random_shuffle(parts.begin(), parts.end());

	/// Посчитаем засечки для каждого куска.
	std::vector<size_t> sum_marks_in_parts(parts.size());
	size_t sum_marks = 0;
	for (size_t i = 0; i < parts.size(); ++i)
	{
		/// Пусть отрезки будут перечислены справа налево, чтобы можно было выбрасывать самый левый отрезок с помощью pop_back().
		std::reverse(parts[i].ranges.begin(), parts[i].ranges.end());

		sum_marks_in_parts[i] = 0;
		for (size_t j = 0; j < parts[i].ranges.size(); ++j)
		{
			MarkRange & range = parts[i].ranges[j];
			sum_marks_in_parts[i] += range.end - range.begin;
		}
		sum_marks += sum_marks_in_parts[i];
	}

	if (sum_marks > max_marks_to_use_cache)
		use_uncompressed_cache = false;

	BlockInputStreams res;

	if (sum_marks > 0)
	{
		size_t min_marks_per_thread = (sum_marks - 1) / threads + 1;

		for (size_t i = 0; i < threads && !parts.empty(); ++i)
		{
			size_t need_marks = min_marks_per_thread;
			BlockInputStreams streams;

			/// Цикл по кускам.
			while (need_marks > 0 && !parts.empty())
			{
				RangesInDataPart & part = parts.back();
				size_t & marks_in_part = sum_marks_in_parts.back();

				/// Не будем брать из куска слишком мало строк.
				if (marks_in_part >= min_marks_for_concurrent_read &&
					need_marks < min_marks_for_concurrent_read)
					need_marks = min_marks_for_concurrent_read;

				/// Не будем оставлять в куске слишком мало строк.
				if (marks_in_part > need_marks &&
					marks_in_part - need_marks < min_marks_for_concurrent_read)
					need_marks = marks_in_part;

				/// Возьмем весь кусок, если он достаточно мал.
				if (marks_in_part <= need_marks)
				{
					/// Восстановим порядок отрезков.
					std::reverse(part.ranges.begin(), part.ranges.end());

					streams.push_back(new MergeTreeBlockInputStream(
						data.getFullPath() + part.data_part->name + '/', max_block_size, column_names, data,
						part.data_part, part.ranges, use_uncompressed_cache,
						prewhere_actions, prewhere_column));
					need_marks -= marks_in_part;
					parts.pop_back();
					sum_marks_in_parts.pop_back();
					continue;
				}

				MarkRanges ranges_to_get_from_part;

				/// Цикл по отрезкам куска.
				while (need_marks > 0)
				{
					if (part.ranges.empty())
						throw Exception("Unexpected end of ranges while spreading marks among threads", ErrorCodes::LOGICAL_ERROR);

					MarkRange & range = part.ranges.back();
					size_t marks_in_range = range.end - range.begin;

					size_t marks_to_get_from_range = std::min(marks_in_range, need_marks);
					ranges_to_get_from_part.push_back(MarkRange(range.begin, range.begin + marks_to_get_from_range));
					range.begin += marks_to_get_from_range;
					marks_in_part -= marks_to_get_from_range;
					need_marks -= marks_to_get_from_range;
					if (range.begin == range.end)
						part.ranges.pop_back();
				}

				streams.push_back(new MergeTreeBlockInputStream(
					data.getFullPath() + part.data_part->name + '/', max_block_size, column_names, data,
					part.data_part, ranges_to_get_from_part, use_uncompressed_cache,
					prewhere_actions, prewhere_column));
			}

			if (streams.size() == 1)
				res.push_back(streams[0]);
			else
				res.push_back(new ConcatBlockInputStream(streams));
		}

		if (!parts.empty())
			throw Exception("Couldn't spread marks among threads", ErrorCodes::LOGICAL_ERROR);
	}

	return res;
}

BlockInputStreams MergeTreeDataSelectExecutor::spreadMarkRangesAmongThreadsFinal(
	RangesInDataParts parts,
	size_t threads,
	const Names & column_names,
	size_t max_block_size,
	bool use_uncompressed_cache,
	ExpressionActionsPtr prewhere_actions,
	const String & prewhere_column)
{
	size_t sum_marks = 0;
	for (size_t i = 0; i < parts.size(); ++i)
		for (size_t j = 0; j < parts[i].ranges.size(); ++j)
			sum_marks += parts[i].ranges[j].end - parts[i].ranges[j].begin;

	if (sum_marks > max_marks_to_use_cache)
		use_uncompressed_cache = false;

	ExpressionActionsPtr sign_filter_expression;
	String sign_filter_column;
	createPositiveSignCondition(sign_filter_expression, sign_filter_column);

	BlockInputStreams to_collapse;

	for (size_t part_index = 0; part_index < parts.size(); ++part_index)
	{
		RangesInDataPart & part = parts[part_index];

		BlockInputStreamPtr source_stream = new MergeTreeBlockInputStream(
			data.getFullPath() + part.data_part->name + '/', max_block_size, column_names, data,
			part.data_part, part.ranges, use_uncompressed_cache,
			prewhere_actions, prewhere_column);

		to_collapse.push_back(new ExpressionBlockInputStream(source_stream, data.getPrimaryExpression()));
	}

	BlockInputStreams res;
	if (to_collapse.size() == 1)
		res.push_back(new FilterBlockInputStream(new ExpressionBlockInputStream(to_collapse[0], sign_filter_expression), sign_filter_column));
	else if (to_collapse.size() > 1)
		res.push_back(new CollapsingFinalBlockInputStream(to_collapse, data.getSortDescription(), data.sign_column));

	return res;
}

void MergeTreeDataSelectExecutor::createPositiveSignCondition(ExpressionActionsPtr & out_expression, String & out_column)
{
	ASTFunction * function = new ASTFunction;
	ASTPtr function_ptr = function;

	ASTExpressionList * arguments = new ASTExpressionList;
	ASTPtr arguments_ptr = arguments;

	ASTIdentifier * sign = new ASTIdentifier;
	ASTPtr sign_ptr = sign;

	ASTLiteral * one = new ASTLiteral;
	ASTPtr one_ptr = one;

	function->name = "equals";
	function->arguments = arguments_ptr;
	function->children.push_back(arguments_ptr);

	arguments->children.push_back(sign_ptr);
	arguments->children.push_back(one_ptr);

	sign->name = data.sign_column;
	sign->kind = ASTIdentifier::Column;

	one->type = new DataTypeInt8;
	one->value = Field(static_cast<Int64>(1));

	out_expression = ExpressionAnalyzer(function_ptr, data.context, data.getColumnsList()).getActions(false);
	out_column = function->getColumnName();
}

/// Получает набор диапазонов засечек, вне которых не могут находиться ключи из заданного диапазона.
MarkRanges MergeTreeDataSelectExecutor::markRangesFromPkRange(const MergeTreeData::DataPart::Index & index, PKCondition & key_condition)
{
	MarkRanges res;

	size_t key_size = data.getSortDescription().size();
	size_t marks_count = index.size() / key_size;

	/// Если индекс не используется.
	if (key_condition.alwaysTrue())
	{
		res.push_back(MarkRange(0, marks_count));
	}
	else
	{
		/** В стеке всегда будут находиться непересекающиеся подозрительные отрезки, самый левый наверху (back).
			* На каждом шаге берем левый отрезок и проверяем, подходит ли он.
			* Если подходит, разбиваем его на более мелкие и кладем их в стек. Если нет - выбрасываем его.
			* Если отрезок уже длиной в одну засечку, добавляем его в ответ и выбрасываем.
			*/
		std::vector<MarkRange> ranges_stack;
		ranges_stack.push_back(MarkRange(0, marks_count));
		while (!ranges_stack.empty())
		{
			MarkRange range = ranges_stack.back();
			ranges_stack.pop_back();

			bool may_be_true;
			if (range.end == marks_count)
				may_be_true = key_condition.mayBeTrueAfter(&index[range.begin * key_size]);
			else
				may_be_true = key_condition.mayBeTrueInRange(&index[range.begin * key_size], &index[range.end * key_size]);

			if (!may_be_true)
				continue;

			if (range.end == range.begin + 1)
			{
				/// Увидели полезный промежуток между соседними засечками. Либо добавим его к последнему диапазону, либо начнем новый диапазон.
				if (res.empty() || range.begin - res.back().end > min_marks_for_seek)
					res.push_back(range);
				else
					res.back().end = range.end;
			}
			else
			{
				/// Разбиваем отрезок и кладем результат в стек справа налево.
				size_t step = (range.end - range.begin - 1) / data.settings.coarse_index_granularity + 1;
				size_t end;

				for (end = range.end; end > range.begin + step; end -= step)
					ranges_stack.push_back(MarkRange(end - step, end));

				ranges_stack.push_back(MarkRange(range.begin, end));
			}
		}
	}

	return res;
}

}