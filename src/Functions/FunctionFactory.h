#pragma once

#include <Interpreters/Context_fwd.h>
#include <Common/register_objects.h>
#include <Common/IFactoryWithAliases.h>
#include <Common/FunctionDocumentation.h>
#include <Functions/IFunction.h>
#include <Functions/IFunctionAdaptors.h>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>


namespace DB
{

using FunctionCreator = std::function<FunctionOverloadResolverPtr(ContextPtr)>;
using FunctionSimpleCreator = std::function<FunctionPtr(ContextPtr)>;
using FunctionFactoryData = std::pair<FunctionCreator, FunctionDocumentation>;

/** Creates function by name.
  * The provided Context is guaranteed to outlive the created function. Functions may use it for
  * things like settings, current database, permission checks, etc.
  */
class FunctionFactory : private boost::noncopyable, public IFactoryWithAliases<FunctionFactoryData>
{
public:
    static FunctionFactory & instance();

    template <typename Function>
    void registerFunction(FunctionDocumentation doc = {}, CaseSensitiveness case_sensitiveness = CaseSensitive)
    {
        registerFunction<Function>(Function::name, std::move(doc), case_sensitiveness);
    }

    template <typename Function>
    void registerFunction(const std::string & name, FunctionDocumentation doc = {}, CaseSensitiveness case_sensitiveness = CaseSensitive)
    {
        if constexpr (std::is_base_of_v<IFunction, Function>)
            registerFunction(name, &adaptFunctionToOverloadResolver<Function>, std::move(doc), case_sensitiveness);
        else
            registerFunction(name, &Function::create, std::move(doc), case_sensitiveness);
    }

    /// This function is used by YQL - innovative transactional DBMS that depends on ClickHouse by source code.
    std::vector<std::string> getAllNames() const;

    bool has(const std::string & name) const;

    /// Throws an exception if not found.
    FunctionOverloadResolverPtr get(const std::string & name, ContextPtr context) const;

    /// Returns nullptr if not found.
    FunctionOverloadResolverPtr tryGet(const std::string & name, ContextPtr context) const;

    /// The same methods to get developer interface implementation.
    FunctionOverloadResolverPtr getImpl(const std::string & name, ContextPtr context) const;
    FunctionOverloadResolverPtr tryGetImpl(const std::string & name, ContextPtr context) const;

    /// Register a function by its name.
    /// No locking, you must register all functions before usage of get.
    void registerFunction(
        const std::string & name,
        FunctionCreator creator,
        FunctionDocumentation doc = {},
        CaseSensitiveness case_sensitiveness = CaseSensitive);

    void registerFunction(
        const std::string & name,
        FunctionSimpleCreator creator,
        FunctionDocumentation doc = {},
        CaseSensitiveness case_sensitiveness = CaseSensitive);

    FunctionDocumentation getDocumentation(const std::string & name) const;

private:
    using Functions = std::unordered_map<std::string, Value>;

    Functions functions;
    Functions case_insensitive_functions;

    template <typename Function>
    static FunctionOverloadResolverPtr adaptFunctionToOverloadResolver(ContextPtr context)
    {
        return std::make_unique<FunctionToOverloadResolverAdaptor>(Function::create(context));
    }

    const Functions & getMap() const override { return functions; }

    const Functions & getCaseInsensitiveMap() const override { return case_insensitive_functions; }

    String getFactoryName() const override { return "FunctionFactory"; }
};

const String & getFunctionCanonicalNameIfAny(const String & name);

}
