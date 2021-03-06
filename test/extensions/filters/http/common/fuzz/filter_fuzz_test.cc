#include <chrono>
#include <memory>

#include "envoy/extensions/filters/network/http_connection_manager/v3/http_connection_manager.pb.h"

#include "common/config/utility.h"
#include "common/config/version_converter.h"
#include "common/protobuf/utility.h"

#include "extensions/filters/http/well_known_names.h"

#include "test/config/utility.h"
#include "test/extensions/filters/http/common/fuzz/filter_fuzz.pb.h"
#include "test/fuzz/fuzz_runner.h"
#include "test/fuzz/utility.h"
#include "test/mocks/buffer/mocks.h"
#include "test/mocks/http/mocks.h"
#include "test/mocks/server/mocks.h"

namespace Envoy {
namespace Extensions {
namespace HttpFilters {

class UberFilterFuzzer {
public:
  UberFilterFuzzer() {
    // Need to set for both a decoder filter and an encoder/decoder filter.
    ON_CALL(filter_callback_, addStreamDecoderFilter(_))
        .WillByDefault(
            Invoke([&](std::shared_ptr<Envoy::Http::StreamDecoderFilter> filter) -> void {
              filter_ = filter;
              filter_->setDecoderFilterCallbacks(callbacks_);
            }));
    ON_CALL(filter_callback_, addStreamFilter(_))
        .WillByDefault(
            Invoke([&](std::shared_ptr<Envoy::Http::StreamDecoderFilter> filter) -> void {
              filter_ = filter;
              filter_->setDecoderFilterCallbacks(callbacks_);
            }));
    // Ext-authz setup
    prepareExtAuthz();
    prepareCache();
  }

  void prepareExtAuthz() {
    // Preparing the expectations for the ext_authz filter.
    addr_ = std::make_shared<Network::Address::Ipv4Instance>("1.2.3.4", 1111);
    ON_CALL(connection_, remoteAddress()).WillByDefault(testing::ReturnRef(addr_));
    ON_CALL(connection_, localAddress()).WillByDefault(testing::ReturnRef(addr_));
    ON_CALL(callbacks_, connection()).WillByDefault(testing::Return(&connection_));
    ON_CALL(callbacks_, activeSpan())
        .WillByDefault(testing::ReturnRef(Tracing::NullSpan::instance()));
    callbacks_.stream_info_.protocol_ = Envoy::Http::Protocol::Http2;
  }

  void prepareCache() {
    // Prepare expectations for dynamic forward proxy.
    ON_CALL(factory_context_.dispatcher_, createDnsResolver(_, _))
        .WillByDefault(testing::Return(resolver_));
  }

  // This executes the decode methods to be fuzzed.
  void decode(Http::StreamDecoderFilter* filter, const test::fuzz::HttpData& data) {
    bool end_stream = false;

    auto headers = Fuzz::fromHeaders<Http::TestRequestHeaderMapImpl>(data.headers());
    if (headers.Path() == nullptr) {
      headers.setPath("/foo");
    }
    if (headers.Method() == nullptr) {
      headers.setMethod("GET");
    }
    if (headers.Host() == nullptr) {
      headers.setHost("foo.com");
    }

    if (data.data().size() == 0 && !data.has_trailers()) {
      end_stream = true;
    }
    ENVOY_LOG_MISC(debug, "Decoding headers: {} ", data.headers().DebugString());
    const auto& headersStatus = filter->decodeHeaders(headers, end_stream);
    if (headersStatus != Http::FilterHeadersStatus::Continue ||
        headersStatus != Http::FilterHeadersStatus::StopIteration) {
      return;
    }

    for (int i = 0; i < data.data().size(); i++) {
      if (i == data.data().size() - 1 && !data.has_trailers()) {
        end_stream = true;
      }
      Buffer::OwnedImpl buffer(data.data().Get(i));
      ENVOY_LOG_MISC(debug, "Decoding data: {} ", buffer.toString());
      if (filter->decodeData(buffer, end_stream) != Http::FilterDataStatus::Continue) {
        return;
      }
    }

    if (data.has_trailers()) {
      ENVOY_LOG_MISC(debug, "Decoding trailers: {} ", data.trailers().DebugString());
      auto trailers = Fuzz::fromHeaders<Http::TestRequestTrailerMapImpl>(data.trailers());
      filter->decodeTrailers(trailers);
    }
  }

  // This creates the filter config and runs decode.
  void fuzz(const envoy::extensions::filters::network::http_connection_manager::v3::HttpFilter&
                proto_config,
            const test::fuzz::HttpData& data) {
    try {
      // Try to create the filter. Exit early if the config is invalid or violates PGV constraints.
      ENVOY_LOG_MISC(info, "filter name {}", proto_config.name());
      auto& factory = Config::Utility::getAndCheckFactoryByName<
          Server::Configuration::NamedHttpFilterConfigFactory>(proto_config.name());
      ProtobufTypes::MessagePtr message = Config::Utility::translateToFactoryConfig(
          proto_config, factory_context_.messageValidationVisitor(), factory);
      cb_ = factory.createFilterFactoryFromProto(*message, "stats", factory_context_);
      cb_(filter_callback_);
    } catch (const EnvoyException& e) {
      ENVOY_LOG_MISC(debug, "Controlled exception {}", e.what());
      return;
    }

    decode(filter_.get(), data);
    reset();
  }

  void reset() {
    if (filter_.get() != nullptr) {
      filter_.get()->onDestroy();
    }
    filter_.reset();
  }

  NiceMock<Server::Configuration::MockFactoryContext> factory_context_;
  NiceMock<Http::MockStreamDecoderFilterCallbacks> callbacks_;
  NiceMock<Http::MockFilterChainFactoryCallbacks> filter_callback_;
  std::shared_ptr<Network::MockDnsResolver> resolver_{std::make_shared<Network::MockDnsResolver>()};
  std::shared_ptr<Http::StreamDecoderFilter> filter_;
  Http::FilterFactoryCb cb_;
  NiceMock<Envoy::Network::MockConnection> connection_;
  Network::Address::InstanceConstSharedPtr addr_;
};

DEFINE_PROTO_FUZZER(const test::extensions::filters::http::FilterFuzzTestCase& input) {
  static PostProcessorRegistration reg = {[](test::extensions::filters::http::FilterFuzzTestCase*
                                                 input,
                                             unsigned int seed) {
    // This ensures that the mutated configs all have valid filter names and type_urls. The list of
    // names and type_urls is pulled from the NamedHttpFilterConfigFactory. All Envoy extensions are
    // built with this test (see BUILD file).
    // This post-processor mutation is applied only when libprotobuf-mutator calls mutate on an
    // input, and *not* during fuzz target execution. Replaying a corpus through the fuzzer will not
    // be affected by the post-processor mutation.
    static const std::vector<absl::string_view> filter_names = Registry::FactoryRegistry<
        Server::Configuration::NamedHttpFilterConfigFactory>::registeredNames();
    static const auto factories =
        Registry::FactoryRegistry<Server::Configuration::NamedHttpFilterConfigFactory>::factories();
    // Choose a valid filter name.
    if (std::find(filter_names.begin(), filter_names.end(), input->config().name()) ==
        std::end(filter_names)) {
      absl::string_view filter_name = filter_names[seed % filter_names.size()];
      input->mutable_config()->set_name(std::string(filter_name));
    }
    // Set the corresponding type_url for Any.
    auto& factory = factories.at(input->config().name());
    input->mutable_config()->mutable_typed_config()->set_type_url(absl::StrCat(
        "type.googleapis.com/", factory->createEmptyConfigProto()->GetDescriptor()->full_name()));
  }};

  // Fuzz filter.
  static UberFilterFuzzer fuzzer;
  fuzzer.fuzz(input.config(), input.data());
}

} // namespace HttpFilters
} // namespace Extensions
} // namespace Envoy
