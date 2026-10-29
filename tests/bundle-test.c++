// Appended to the host translation unit by scripts/build.py so admission can be
// exercised without starting V8 or opening sockets.
int main() {
  using namespace isolate_host;
  auto decode = [](IsolateWorkerSource::Builder source) {
    capnp::MallocMessageBuilder message;
    message.setRoot(source.asReader());
    kj::VectorOutputStream bytes;
    capnp::writePackedMessage(bytes, message);
    return decodeWorkerBundle(bytes.getArray());
  };
  auto expectFailure = [](auto&& operation) {
    KJ_REQUIRE(kj::runCatchingExceptions(kj::fwd<decltype(operation)>(operation)) != kj::none,
        "invalid bundle was admitted");
  };
  capnp::MallocMessageBuilder message;
  auto source = message.initRoot<IsolateWorkerSource>();
  source.setFormatVersion(1);
  source.setMainModule("main.js");
  source.setCompatibilityDate("2026-06-10");
  auto modules = source.initModules(2);
  modules[0].setName("main.js");
  modules[0].setEsModule(kj::StringPtr("export default {}").asBytes());
  modules[1].setName("capnp:/example/service");
  modules[1].setText(kj::StringPtr("opaque module").asBytes());
  auto bindings = source.initBindings(2);
  bindings[0].setName("HTTP");
  bindings[0].setHttpService("arbitrary-service");
  bindings[1].setName("RPC");
  bindings[1].setCapnp("arbitrary-bootstrap");
  auto result = decode(source);
  KJ_REQUIRE(result->modules.size() == 2 && result->bindings.size() == 2);
  KJ_REQUIRE(result->bindings[0].value == "arbitrary-service");
  KJ_REQUIRE(result->bindings[1].value == "arbitrary-bootstrap");
  KJ_REQUIRE(result->modules[1].name == "capnp:/example/service");
  KJ_REQUIRE(isValidWorkerId("a/b") && isValidWorkerId("x") && !isValidWorkerId(""));
  KJ_REQUIRE(!isValidWorkerId(kj::str(kj::repeat('x', 257))));
  bindings[1].setCapnp("");
  expectFailure([&]() { decode(source); });
  bindings[1].setCapnp("arbitrary-bootstrap");
  bindings[1].setName("HTTP");
  expectFailure([&]() { decode(source); });
  bindings[1].setName("RPC");
  source.setFormatVersion(2);
  expectFailure([&]() { decode(source); });
  source.setFormatVersion(1);
  modules[1].setName("main.js");
  expectFailure([&]() { decode(source); });
  modules[1].setName("capnp:/example/service");
  source.setMainModule("missing.js");
  expectFailure([&]() { decode(source); });
  source.setMainModule("main.js");
  bindings[1].setJson(kj::StringPtr("{invalid}").asBytes());
  expectFailure([&]() { decode(source); });
  puts("bundle validation passed");
}
