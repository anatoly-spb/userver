#pragma once

#include <functional>
#include <vector>

#include <cache/cache_update_trait.hpp>
#include <concurrent/variable.hpp>
#include <engine/mutex.hpp>
#include <server/handlers/http_handler_json_base.hpp>

namespace components {
class TestsuiteSupport;
}  // namespace components

namespace server {
namespace handlers {

class TestsControl final : public HttpHandlerJsonBase {
 public:
  TestsControl(const components::ComponentConfig& config,
               const components::ComponentContext& component_context);

  static constexpr const char* kName = "tests-control";

  const std::string& HandlerName() const override;
  formats::json::Value HandleRequestJsonThrow(
      const http::HttpRequest& request,
      const formats::json::Value& request_body,
      request::RequestContext& context) const override;

 private:
  formats::json::Value ActionRunPeriodicTask(
      const formats::json::Value& request_body) const;

  concurrent::Variable<std::reference_wrapper<components::TestsuiteSupport>>
      testsuite_support_;
};

}  // namespace handlers
}  // namespace server
