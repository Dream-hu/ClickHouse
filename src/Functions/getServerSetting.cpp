#include <Functions/IFunction.h>
#include <Functions/FunctionFactory.h>
#include <Functions/FunctionHelpers.h>
#include <DataTypes/FieldToDataType.h>
#include <Interpreters/convertFieldToType.h>
#include <Interpreters/Context.h>
#include <Core/Field.h>
#include <Core/ServerSettings.h>


namespace DB
{
namespace ErrorCodes
{
    extern const int ILLEGAL_TYPE_OF_ARGUMENT;
    extern const int ILLEGAL_COLUMN;
}

namespace
{

class FunctionGetServerSetting : public IFunction, WithContext
{
public:
    static constexpr auto name = "getServerSetting";

    static FunctionPtr create(ContextPtr context_) { return std::make_shared<FunctionGetServerSetting>(context_); }
    explicit FunctionGetServerSetting(ContextPtr context_) : WithContext(context_) {}

    String getName() const override { return name; }

    bool isDeterministic() const override { return false; }

    bool isSuitableForShortCircuitArgumentsExecution(const DataTypesWithConstInfo & /*arguments*/) const override { return false; }

    size_t getNumberOfArguments() const override { return 1 ; }

    ColumnNumbers getArgumentsThatAreAlwaysConstant() const override { return {0}; }

    DataTypePtr getReturnTypeImpl(const ColumnsWithTypeAndName & arguments) const override
    {
        if (arguments.size() != 1)
            throw Exception(
                ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                "Number of arguments for function {} can't be {}, should be 1",
                getName(),
                arguments.size());

        if (!isString(arguments[0].type))
            throw Exception(
                ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                "The argument of function {} should be a constant string with the name of a setting",
                String{name});
        const auto * column = arguments[0].column.get();

        if (!column || !checkAndGetColumnConstStringOrFixedString(column))
            throw Exception(
                ErrorCodes::ILLEGAL_COLUMN,
                "The argument of function {} should be a constant string with the name of a setting",
                String{name});

        std::string_view setting_name{column->getDataAt(0).toView()};
        auto value = getContext()->getServerSettings().get(setting_name);

        return applyVisitor(FieldToDataType{}, value);
    }

    ColumnPtr executeImpl(const ColumnsWithTypeAndName & arguments, const DataTypePtr & result_type, size_t input_rows_count) const override
    {
        if (arguments.size() != 1)
            throw Exception(
                ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                "Number of arguments for function {} can't be {}, should be 1",
                getName(),
                arguments.size());

        if (!isString(arguments[0].type))
            throw Exception(
                ErrorCodes::ILLEGAL_TYPE_OF_ARGUMENT,
                "The argument of function {} should be a constant string with the name of a setting",
                String{name});
        const auto * column = arguments[0].column.get();

        if (!column || !checkAndGetColumnConstStringOrFixedString(column))
            throw Exception(
                ErrorCodes::ILLEGAL_COLUMN,
                "The argument of function {} should be a constant string with the name of a setting",
                String{name});

        std::string_view setting_name{column->getDataAt(0).toView()};
        auto value = getContext()->getServerSettings().get(setting_name);

        return result_type->createColumnConst(input_rows_count, convertFieldToType(value, *result_type));
    }
};

}

REGISTER_FUNCTION(GetServerSetting)
{
    factory.registerFunction<FunctionGetServerSetting>(FunctionDocumentation{
        .description = R"(
Returns the current value of server setting.
)",
        .syntax = "getServerSetting('server_setting')",
        .arguments = {
            {"server_setting", "The setting name. Type: String."}
        },
        .returned_value = "The setting's current value.",
        .examples = {
            {"getServerSetting", "SELECT getSetting('page_cache_size_ratio');", "SRLU"},
        },
        .category{"Other"}}, FunctionFactory::Case::Sensitive);
}

}
