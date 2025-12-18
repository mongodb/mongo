// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <functional>

#include "opentelemetry/sdk/instrumentationscope/instrumentation_scope.h"
#include "opentelemetry/version.h"

OPENTELEMETRY_BEGIN_NAMESPACE
namespace sdk
{
namespace instrumentationscope
{
/**
 * A scope configurator is a function that returns the scope config for a given instrumentation
 * scope.
 */
template <typename T>
class ScopeConfigurator
{
public:
  /**
   * A builder class for the ScopeConfigurator that facilitates the creation of ScopeConfigurators.
   */
  class Builder
  {
  public:
    /**
     * Constructor for a builder object that cam be used to create a scope configurator. A minimally
     * configured builder would build a ScopeConfigurator that applies the default_scope_config to
     * every instrumentation scope.
     * @param default_scope_config The default scope config that the built configurator should fall
     * back on.
     */
    explicit Builder(T default_scope_config) noexcept : default_scope_config_(default_scope_config)
    {}

    /**
     * Allows the user to pass a generic function that evaluates an instrumentation scope through a
     * boolean check. If the check passes, the provided config is applied. Conditions are evaluated
     * in order.
     * @param scope_matcher a function that returns true if the scope being evaluated matches the
     * criteria defined by the function.
     * @param scope_config the scope configuration to return for the matched scope.
     * @return this
     */
    Builder &AddCondition(std::function<bool(const InstrumentationScope &)> scope_matcher,
                          T scope_config)
    {
      conditions_.emplace_back(scope_matcher, scope_config);
      return *this;
    }

    /**
     * A convenience condition that specifically matches the scope name of the scope being
     * evaluated. If the scope name matches to the provided string, then the provided scope
     * configuration is applied to the scope.
     * @param scope_name The scope name to which the config needs to be applied.
     * @param scope_config The scope config for the matching scopes.
     * @return this
     */
    Builder &AddConditionNameEquals(nostd::string_view scope_name, T scope_config)
    {
      std::function<bool(const InstrumentationScope &)> name_equals_matcher =
          [scope_name = std::string(scope_name)](const InstrumentationScope &scope_info) {
            return scope_info.GetName() == scope_name;
          };
      conditions_.emplace_back(name_equals_matcher, scope_config);
      return *this;
    }

    /**
     * Constructs the scope configurator object that can be used to retrieve scope config depending
     * on the instrumentation scope.
     * @return a configured scope configurator.
     */
    ScopeConfigurator<T> Build() const
    {
      if (conditions_.size() == 0)
      {
        return ScopeConfigurator<T>(
            [default_scope_config_ = this->default_scope_config_](const InstrumentationScope &) {
              return default_scope_config_;
            });
      }

      // Return a configurator that processes all the conditions
      return ScopeConfigurator<T>(
          [conditions_ = this->conditions_, default_scope_config_ = this->default_scope_config_](
              const InstrumentationScope &scope_info) {
            for (Condition condition : conditions_)
            {
              if (condition.scope_matcher(scope_info))
              {
                return condition.scope_config;
              }
            }
            return default_scope_config_;
          });
    }

  private:
    /**
     * An internal struct to encapsulate 'conditions' that can be applied to a
     * ScopeConfiguratorBuilder. The applied conditions influence the behavior of the generated
     * ScopeConfigurator.
     */
    struct Condition
    {
      std::function<bool(const InstrumentationScope &)> scope_matcher;
      T scope_config;

      Condition(const std::function<bool(const InstrumentationScope &)> &matcher, const T &config)
          : scope_matcher(matcher), scope_config(config)
      {}
    };

    T default_scope_config_;
    std::vector<Condition> conditions_;
  };

  // Public methods for ScopeConfigurator

  /**
   * Invokes the underlying configurator function to get a valid scope configuration.
   * @param scope_info The InstrumentationScope containing scope information for which configuration
   * needs to be retrieved.
   */
  T ComputeConfig(const InstrumentationScope &scope_info) const
  {
    return this->configurator_(scope_info);
  }

private:
  // Prevent direct initialization of ScopeConfigurator objects.
  explicit ScopeConfigurator(std::function<T(const InstrumentationScope &)> configurator)
      : configurator_(configurator)
  {}

  std::function<T(const InstrumentationScope &)> configurator_;
};
}  // namespace instrumentationscope
}  // namespace sdk
OPENTELEMETRY_END_NAMESPACE
