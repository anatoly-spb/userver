#pragma once

#include <components/component_config.hpp>
#include <components/component_context.hpp>
#include <components/loggable_component_base.hpp>
#include <taxi_config/config.hpp>
#include <utils/fast_pimpl.hpp>

namespace congestion_control {

class Component final : public components::LoggableComponentBase {
 public:
  static constexpr const char* kName = "congestion-control";

  Component(const components::ComponentConfig&,
            const components::ComponentContext&);

  ~Component() override;

 private:
  void OnConfigUpdate(const std::shared_ptr<const taxi_config::Config>& cfg);

  struct Impl;
  utils::FastPimpl<Impl, 408, 8> pimpl_;
};

}  // namespace congestion_control