// Sandstorm - Personal Cloud Sandbox
// Copyright (c) 2026 Sandstorm contributors
// Licensed under the Apache License, Version 2.0.

#include "isolate-host.capnp.h"
#include "isolate-worker-source.capnp.h"

#include <capnp/ez-rpc.h>
#include <capnp/compat/http-over-capnp.h>
#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include <capnp/serialize.h>
#include <capnp/rpc.capnp.h>
#include <kj/debug.h>
#include <kj/function.h>
#include <unistd.h>

namespace isolate_host {
namespace {

class TestStorageService final: public kj::HttpService {
public:
  TestStorageService(kj::HttpHeaderTable& headerTable, kj::String& stored)
      : headerTable(headerTable), stored(stored) {}

  kj::Promise<void> request(kj::HttpMethod method, kj::StringPtr url,
      const kj::HttpHeaders&, kj::AsyncInputStream& requestBody,
      kj::HttpService::Response& response) override {
    if (url.endsWith("/url-test?q=1")) return send(response, url);
    if (method == kj::HttpMethod::PUT) {
      return requestBody.readAllText().then([this, &response](kj::String body) {
        stored = kj::mv(body);
        return send(response, stored);
      });
    }
    KJ_REQUIRE(method == kj::HttpMethod::GET, "unexpected test storage method");
    return send(response, stored);
  }

private:
  kj::HttpHeaderTable& headerTable;
  kj::String& stored;

  kj::Promise<void> send(kj::HttpService::Response& response, kj::StringPtr body) {
    kj::HttpHeaders headers(headerTable);
    auto stream = response.send(200, "OK", headers, body.size());
    return stream->write(body.begin(), body.size()).attach(kj::mv(stream));
  }
};

class BindingServicesImpl final: public IsolateBindingServices::Server {
public:
  BindingServicesImpl()
      : httpFactory(byteStreamFactory, headerTableBuilder),
        headerTable(headerTableBuilder.build()) {}

  kj::Promise<void> getService(GetServiceContext context) override {
    KJ_REQUIRE(context.getParams().getName() == "storage",
        "control test requested an unexpected binding service");
    context.getResults().setService(httpFactory.kjToCapnp(
        kj::heap<TestStorageService>(*headerTable, stored)));
    return kj::READY_NOW;
  }

  kj::Promise<void> getBridge(GetBridgeContext context) override {
    KJ_REQUIRE(context.getParams().getName() == "example-bootstrap",
        "unexpected native bootstrap name");
    context.getResults().setBridge(httpFactory.kjToCapnp(
        kj::heap<TestStorageService>(*headerTable, stored)));
    return kj::READY_NOW;
  }

public:
  kj::StringPtr getStored() const { return stored; }

private:
  capnp::ByteStreamFactory byteStreamFactory;
  kj::HttpHeaderTable::Builder headerTableBuilder;
  capnp::HttpOverCapnpFactory httpFactory;
  kj::Own<kj::HttpHeaderTable> headerTable;
  kj::String stored = kj::str("shared-storage-ok");
};

void expectFailure(kj::Function<void()> operation) {
  auto exception = kj::runCatchingExceptions(kj::mv(operation));
  KJ_REQUIRE(exception != nullptr, "host operation unexpectedly succeeded");
}

}  // namespace
}  // namespace isolate_host

int main(int argc, char** argv) {
  bool inheritedControl = argc == 3 && kj::StringPtr(argv[1]) == "--control-fd"_kj;
  KJ_REQUIRE(inheritedControl || argc == 2 || (argc == 3 && kj::StringPtr(argv[2]) == "--idle-eviction"_kj),
      "usage: isolate-host-client <control-socket-path> [--idle-eviction]");
  bool idleEvictionOnly = argc == 3 && !inheritedControl;
  capnp::MallocMessageBuilder sourceMessage;
  auto source = sourceMessage.initRoot<isolate_host::IsolateWorkerSource>();
  source.setFormatVersion(1);
  source.setMainModule("main.js");
  source.setCompatibilityDate("2026-06-10");
  auto module = source.initModules(1)[0];
  module.setName("main.js");
  capnp::MallocMessageBuilder bootstrapMessage;
  bootstrapMessage.initRoot<capnp::rpc::Message>().initBootstrap().setQuestionId(0);
  auto bootstrapBytes = capnp::messageToFlatArray(bootstrapMessage);
  kj::String bootstrapArray = kj::str("");
  for (auto byte: bootstrapBytes.asBytes()) bootstrapArray = kj::str(bootstrapArray, byte, ",");
  auto script = kj::str("const BOOTSTRAP = new Uint8Array([", bootstrapArray, "]);\n", R"JS(
export default {
  async fetch(request, env) {
    console.log("isolate-host-log-marker");
    if (new URL(request.url).pathname === "/url-test") {
      return env.STORAGE.fetch("https://example.invalid:8443/url-test?q=1");
    }
    if (new URL(request.url).pathname === "/stop-stream") {
      return new Response(new ReadableStream({ async start(c) {
        // Keep a native session alive too, with receive() pending during stop.
        const channel = env.ARBITRARY_RPC.open();
        channel.send(BOOTSTRAP);
        await channel.receive();
        channel.receive().catch(() => {});
        c.enqueue(new TextEncoder().encode("started"));
        await new Promise(resolve => setTimeout(resolve, 300));
        await env.STORAGE.fetch("http://storage/", {method: "PUT", body: "after-stop"});
        c.enqueue(new TextEncoder().encode("finished"));
        c.close();
      }}));
    }
    if (new URL(request.url).pathname === "/websocket") {
      const pair = new WebSocketPair();
      pair[1].accept();
      pair[1].addEventListener("message", e => pair[1].send(e.data));
      return new Response(null, { status: 101, webSocket: pair[0] });
    }
    if (new URL(request.url).pathname === "/bridge-test") {
      const channel = env.ARBITRARY_RPC.open();
      channel.send(BOOTSTRAP);
      const reply = await channel.receive();
      channel.close();
      return new Response(reply.byteLength > 0 ? "bridge-ok" : "empty-reply");
    }
    if (new URL(request.url).pathname === "/bindings-test") {
      const own = await env.SELF.fetch("https://worker.invalid/");
      return new Response(env.MESSAGE + ":" + env.SETTINGS.enabled + ":" +
        new TextDecoder().decode(env.BYTES) + ":" + await own.text());
    }
    if (new URL(request.url).pathname === "/cpu-loop") {
      while (true) {}
    }
    if (new URL(request.url).pathname === "/storage-test") {
      if (request.headers.get("X-Sandstorm-Ingress-Test") !== "ingress-ok") {
        return new Response("missing ingress header", { status: 400 });
      }
      const put = await env.STORAGE.fetch("http://storage/shared-host-test", {
        method: "PUT",
        body: "shared-storage-ok",
      });
      if (!put.ok) return new Response(`storage PUT failed: ${put.status}`, { status: 500 });
      const get = await env.STORAGE.fetch("http://storage/shared-host-test");
      const headers = new Headers(get.headers);
      headers.set("X-Sandstorm-Egress-Test", "egress-ok");
      return new Response(get.body, { status: get.status, headers });
    }
    return new Response("ok");
  },
};
)JS");
  module.setEsModule(script.asBytes());
  auto bindings = source.initBindings(6);
  bindings[0].setName("MESSAGE");
  bindings[0].setText(kj::StringPtr("hello").asBytes());
  bindings[1].setName("SETTINGS");
  bindings[1].setJson(kj::StringPtr("{\"enabled\":true}").asBytes());
  bindings[2].setName("STORAGE");
  bindings[2].setHttpService("storage");
  bindings[3].setName("ARBITRARY_RPC");
  bindings[3].setCapnp("example-bootstrap");
  bindings[4].setName("BYTES");
  bindings[4].setData(kj::StringPtr("binary").asBytes());
  bindings[5].setName("SELF");
  bindings[5].setService("main");
  kj::VectorOutputStream sourceOutput;
  capnp::writePackedMessage(sourceOutput, sourceMessage);
  auto sourceBytes = sourceOutput.getArray();

  capnp::MallocMessageBuilder memorySourceMessage;
  auto memorySource = memorySourceMessage.initRoot<isolate_host::IsolateWorkerSource>();
  memorySource.setFormatVersion(1);
  memorySource.setMainModule("main.js");
  memorySource.setCompatibilityDate("2026-06-10");
  auto memoryModule = memorySource.initModules(1)[0];
  memoryModule.setName("main.js");
  memoryModule.setEsModule(kj::StringPtr(R"JS(
const allocations = [];
while (true) allocations.push(new Array(1024 * 1024).fill(allocations.length));
export default { fetch() { return new Response("memory limit failed"); } };
)JS").asBytes());
  kj::VectorOutputStream memoryOutput;
  capnp::writePackedMessage(memoryOutput, memorySourceMessage);
  auto memoryBytes = memoryOutput.getArray();

  capnp::MallocMessageBuilder invalidMessage;
  auto invalidSource = invalidMessage.initRoot<isolate_host::IsolateWorkerSource>();
  invalidSource.setFormatVersion(1);
  invalidSource.setMainModule("main.js");
  invalidSource.setCompatibilityDate("2026-06-10");
  auto invalidModule = invalidSource.initModules(1)[0];
  invalidModule.setName("main.js");
  invalidModule.setEsModule(script.asBytes());
  auto invalidBinding = invalidSource.initBindings(1)[0];
  invalidBinding.setName("BROKEN");
  invalidBinding.setJson(kj::StringPtr("{not-json}").asBytes());
  kj::VectorOutputStream invalidOutput;
  capnp::writePackedMessage(invalidOutput, invalidMessage);
  auto invalidBytes = invalidOutput.getArray();

  source.setFormatVersion(2);
  kj::VectorOutputStream unsupportedOutput;
  capnp::writePackedMessage(unsupportedOutput, sourceMessage);
  auto unsupportedBytes = unsupportedOutput.getArray();

  kj::Own<capnp::EzRpcClient> connection;
  if (inheritedControl) {
    connection = kj::heap<capnp::EzRpcClient>(kj::StringPtr(argv[2]).parseAs<int>());
  } else {
    connection = kj::heap<capnp::EzRpcClient>(kj::str("unix:", argv[1]));
  }
  auto& rpc = *connection;
  auto& waitScope = rpc.getWaitScope();
  auto host = rpc.getMain<isolate_host::IsolateHost>();
  auto info = host.getInfoRequest().send().wait(waitScope);
  KJ_REQUIRE(info.getProtocolVersion() == 1 && info.getSourceFormatVersion() == 1);
  auto bindingServices = kj::heap<isolate_host::BindingServicesImpl>();
  auto& bindingState = *bindingServices;
  isolate_host::IsolateBindingServices::Client services = kj::mv(bindingServices);

  auto start = host.startWorkerRequest();
  start.setWorkerId("testgrain123");
  start.setServices(services);
  start.setWorkerSource(sourceBytes);
  auto grain = start.send().wait(waitScope).getWorker();
  grain.keepAliveRequest().send().wait(waitScope);

  capnp::ByteStreamFactory byteStreamFactory;
  kj::HttpHeaderTable::Builder headerTableBuilder;
  capnp::HttpOverCapnpFactory httpFactory(byteStreamFactory, headerTableBuilder);
  auto ingressTestHeader = headerTableBuilder.add("X-Sandstorm-Ingress-Test");
  auto egressTestHeader = headerTableBuilder.add("X-Sandstorm-Egress-Test");
  auto headerTable = headerTableBuilder.build();
  auto getHttp = grain.getHttpServiceRequest().send().wait(waitScope);
  auto service = httpFactory.capnpToKj(getHttp.getService());
  auto httpClient = kj::newHttpClient(*service);

  if (idleEvictionOnly) {
    // The host runs in another process, so its timer advances while this client sleeps without
    // driving its own event loop.
    usleep(120 * 1000);
    grain.keepAliveRequest().send().wait(waitScope);
    usleep(120 * 1000);
    kj::HttpHeaders refreshedHeaders(*headerTable);
    auto refreshedRequest = httpClient->request(
        kj::HttpMethod::GET, "https://grain.invalid/", refreshedHeaders);
    auto refreshedResponse = refreshedRequest.response.wait(waitScope);
    KJ_REQUIRE(refreshedResponse.statusCode == 200,
        "keepalive did not extend the hosted grain idle deadline",
        refreshedResponse.statusCode);
    KJ_REQUIRE(refreshedResponse.body->readAllText().wait(waitScope) == "ok",
        "hosted grain returned the wrong response after keepalive");

    usleep(220 * 1000);
    isolate_host::expectFailure([&]() {
      grain.keepAliveRequest().send().wait(waitScope);
    });
    isolate_host::expectFailure([&]() {
      kj::HttpHeaders staleHeaders(*headerTable);
      auto staleRequest = httpClient->request(
          kj::HttpMethod::GET, "https://grain.invalid/", staleHeaders);
      (void)staleRequest.response.wait(waitScope);
    });

    auto restart = host.startWorkerRequest();
    restart.setWorkerId("testgrain123");
    restart.setServices(services);
    restart.setWorkerSource(sourceBytes);
    auto restarted = restart.send().wait(waitScope).getWorker();
    auto restartedHttp = restarted.getHttpServiceRequest().send().wait(waitScope);
    auto restartedService = httpFactory.capnpToKj(restartedHttp.getService());
    auto restartedClient = kj::newHttpClient(*restartedService);
    kj::HttpHeaders restartedHeaders(*headerTable);
    auto restartedRequest = restartedClient->request(
        kj::HttpMethod::GET, "https://grain.invalid/", restartedHeaders);
    auto restartedResponse = restartedRequest.response.wait(waitScope);
    KJ_REQUIRE(restartedResponse.statusCode == 200,
        "idle-evicted grain did not restart", restartedResponse.statusCode);
    KJ_REQUIRE(restartedResponse.body->readAllText().wait(waitScope) == "ok",
        "idle-evicted grain returned the wrong response after restart");
    restarted.stopRequest().send().wait(waitScope);
    return 0;
  }

  kj::HttpHeaders requestHeaders(*headerTable);
  requestHeaders.set(ingressTestHeader, "ingress-ok"_kj);
  auto httpRequest = httpClient->request(
      kj::HttpMethod::GET, "https://grain.invalid/storage-test", requestHeaders);
  auto httpResponse = httpRequest.response.wait(waitScope);
  KJ_REQUIRE(httpResponse.statusCode == 200, "hosted worker request failed", httpResponse.statusCode);
  KJ_REQUIRE(httpResponse.headers->get(egressTestHeader) == "egress-ok"_kj,
      "hosted worker response header was not preserved");
  KJ_REQUIRE(httpResponse.body->readAllText().wait(waitScope) == "shared-storage-ok",
      "hosted worker did not round-trip through its storage binding");

  for (auto path: {"bridge-test", "bindings-test"}) {
    auto http = grain.getHttpServiceRequest().send().wait(waitScope);
    auto service = httpFactory.capnpToKj(http.getService());
    auto client = kj::newHttpClient(*service);
    kj::HttpHeaders headers(*headerTable);
    auto request = client->request(kj::HttpMethod::GET,
        kj::str("https://worker.invalid/", path), headers);
    auto response = request.response.wait(waitScope);
    KJ_REQUIRE(response.statusCode == 200);
    auto body = response.body->readAllText().wait(waitScope);
    KJ_REQUIRE(body == (kj::StringPtr(path) == "bridge-test"
        ? "bridge-ok" : "hello:true:binary:ok"), body);
  }
  {
    kj::HttpHeaders headers(*headerTable);
    auto request = httpClient->request(kj::HttpMethod::GET,
        "https://worker.invalid/url-test", headers);
    auto response = request.response.wait(waitScope);
    KJ_REQUIRE(response.body->readAllText().wait(waitScope) ==
        "https://example.invalid:8443/url-test?q=1", "binding URL was changed");
  }
  // Stop must cancel active streams, including native channels opened by them,
  // and prevent delayed storage writes. A replacement can safely reuse the ID.
  for (auto route: {"stop-stream", "websocket"}) {
    auto start = host.startWorkerRequest();
    start.setWorkerId("stop-probe");
    start.setServices(services);
    start.setWorkerSource(sourceBytes);
    auto worker = start.send().wait(waitScope).getWorker();
    auto http = worker.getHttpServiceRequest().send().wait(waitScope);
    auto service = httpFactory.capnpToKj(http.getService());
    auto client = kj::newHttpClient(*service);
    kj::HttpHeaders headers(*headerTable);
    if (kj::StringPtr(route) == "stop-stream") {
      auto request = client->request(kj::HttpMethod::GET,
          "https://worker.invalid/stop-stream", headers);
      auto response = request.response.wait(waitScope);
      char prefix[7];
      response.body->read(prefix, 7, 7).wait(waitScope);
      worker.stopRequest().send().wait(waitScope);
      isolate_host::expectFailure([&]() { response.body->readAllText().wait(waitScope); });
    } else {
      auto response = client->openWebSocket("https://worker.invalid/websocket", headers)
          .wait(waitScope);
      KJ_REQUIRE(response.statusCode == 101);
      auto& socket = response.webSocketOrBody.get<kj::Own<kj::WebSocket>>();
      socket->send("echo"_kj).wait(waitScope);
      auto echo = socket->receive().wait(waitScope);
      KJ_REQUIRE(echo.get<kj::String>() == "echo");
      worker.stopRequest().send().wait(waitScope);
      isolate_host::expectFailure([&]() { socket->receive().wait(waitScope); });
    }
  }
  rpc.getIoProvider().getTimer().afterDelay(400 * kj::MILLISECONDS).wait(waitScope);
  KJ_REQUIRE(bindingState.getStored() == "shared-storage-ok",
      "stopped worker performed a delayed storage write");
  // IDs have no filesystem meaning; retain the same running worker for any opaque ID.
  auto opaque = host.startWorkerRequest();
  opaque.setWorkerId("a/b");
  opaque.setServices(services);
  opaque.setWorkerSource(sourceBytes);
  auto opaqueWorker = opaque.send().wait(waitScope).getWorker();
  opaqueWorker.stopRequest().send().wait(waitScope);
  auto cpuStart = host.startWorkerRequest();
  cpuStart.setWorkerId("cpugrain123");
  cpuStart.setServices(services);
  cpuStart.setWorkerSource(sourceBytes);
  auto cpuGrain = cpuStart.send().wait(waitScope).getWorker();
  auto cpuHttp = cpuGrain.getHttpServiceRequest().send().wait(waitScope);
  auto cpuService = httpFactory.capnpToKj(cpuHttp.getService());
  auto cpuClient = kj::newHttpClient(*cpuService);
  kj::HttpHeaders cpuHeaders(*headerTable);
  auto cpuRequest = cpuClient->request(
      kj::HttpMethod::GET, "https://grain.invalid/cpu-loop", cpuHeaders);
  bool cpuRejected = false;
  auto cpuFailure = kj::runCatchingExceptions([&]() {
    auto response = cpuRequest.response.wait(waitScope);
    cpuRejected = response.statusCode >= 500;
  });
  if (cpuFailure != nullptr) cpuRejected = true;
  KJ_REQUIRE(cpuRejected, "CPU watchdog did not terminate the worker request");

  // A condemned grain must not stall or poison unrelated isolates in the shared process.
  kj::HttpHeaders neighborHeaders(*headerTable);
  auto neighborRequest = httpClient->request(
      kj::HttpMethod::GET, "https://grain.invalid/", neighborHeaders);
  auto neighborResponse = neighborRequest.response.wait(waitScope);
  KJ_REQUIRE(neighborResponse.statusCode == 200,
      "CPU watchdog affected an unrelated worker", neighborResponse.statusCode);
  KJ_REQUIRE(neighborResponse.body->readAllText().wait(waitScope) == "ok",
      "unrelated worker returned the wrong response after watchdog termination");
  cpuGrain.stopRequest().send().wait(waitScope);

  isolate_host::expectFailure([&]() {
    auto memoryStart = host.startWorkerRequest();
    memoryStart.setWorkerId("memorygrain123");
    memoryStart.setServices(services);
    memoryStart.setWorkerSource(memoryBytes);
    auto memoryGrain = memoryStart.send().wait(waitScope).getWorker();
    auto memoryHttp = memoryGrain.getHttpServiceRequest().send().wait(waitScope);
    auto memoryService = httpFactory.capnpToKj(memoryHttp.getService());
    auto memoryClient = kj::newHttpClient(*memoryService);
    kj::HttpHeaders memoryHeaders(*headerTable);
    auto memoryRequest = memoryClient->request(
        kj::HttpMethod::GET, "https://grain.invalid/", memoryHeaders);
    auto memoryResponse = memoryRequest.response.wait(waitScope);
    KJ_REQUIRE(memoryResponse.statusCode < 500, "memory-bound worker request rejected");
  });

  kj::HttpHeaders postMemoryHeaders(*headerTable);
  auto postMemoryRequest = httpClient->request(
      kj::HttpMethod::GET, "https://grain.invalid/", postMemoryHeaders);
  auto postMemoryResponse = postMemoryRequest.response.wait(waitScope);
  KJ_REQUIRE(postMemoryResponse.statusCode == 200,
      "heap-limit termination affected an unrelated worker", postMemoryResponse.statusCode);
  grain.stopRequest().send().wait(waitScope);

  isolate_host::expectFailure([&]() {
    grain.keepAliveRequest().send().wait(waitScope);
  });
  isolate_host::expectFailure([&]() {
    kj::HttpHeaders staleHeaders(*headerTable);
    auto staleRequest = httpClient->request(
        kj::HttpMethod::GET, "https://grain.invalid/", staleHeaders);
    (void)staleRequest.response.wait(waitScope);
  });

  auto restart = host.startWorkerRequest();
  restart.setWorkerId("testgrain123");
  restart.setServices(services);
  restart.setWorkerSource(sourceBytes);
  auto restartedGrain = restart.send().wait(waitScope).getWorker();
  restartedGrain.keepAliveRequest().send().wait(waitScope);
  auto restartedHttp = restartedGrain.getHttpServiceRequest().send().wait(waitScope);
  auto restartedService = httpFactory.capnpToKj(restartedHttp.getService());
  auto restartedClient = kj::newHttpClient(*restartedService);
  kj::HttpHeaders restartedHeaders(*headerTable);
  restartedHeaders.set(ingressTestHeader, "ingress-ok"_kj);
  auto restartedRequest = restartedClient->request(
      kj::HttpMethod::GET, "https://grain.invalid/storage-test", restartedHeaders);
  auto restartedResponse = restartedRequest.response.wait(waitScope);
  KJ_REQUIRE(restartedResponse.statusCode == 200,
      "restarted hosted worker request failed", restartedResponse.statusCode);
  KJ_REQUIRE(restartedResponse.headers->get(egressTestHeader) == "egress-ok"_kj,
      "restarted hosted worker response header was not preserved");
  KJ_REQUIRE(restartedResponse.body->readAllText().wait(waitScope) == "shared-storage-ok",
      "restarted worker lost its STORAGE capability");
  isolate_host::expectFailure([&]() {
    grain.keepAliveRequest().send().wait(waitScope);
  });
  restartedGrain.stopRequest().send().wait(waitScope);
  isolate_host::expectFailure([&]() {
    auto invalid = host.startWorkerRequest();
    invalid.setWorkerId("");
    invalid.setWorkerSource(sourceBytes);
    invalid.setServices(services);
    invalid.send().wait(waitScope);
  });
  isolate_host::expectFailure([&]() {
    auto incomplete = host.startWorkerRequest();
    incomplete.setWorkerId("missingworker123");
    incomplete.setServices(services);
    incomplete.send().wait(waitScope);
  });
  isolate_host::expectFailure([&]() {
    auto invalid = host.startWorkerRequest();
    invalid.setWorkerId("invalidjson");
    invalid.setServices(services);
    invalid.setWorkerSource(invalidBytes);
    invalid.send().wait(waitScope);
  });
  isolate_host::expectFailure([&]() {
    auto unsupported = host.startWorkerRequest();
    unsupported.setWorkerId("unsupportedversion");
    unsupported.setServices(services);
    unsupported.setWorkerSource(unsupportedBytes);
    unsupported.send().wait(waitScope);
  });
  isolate_host::expectFailure([&]() {
    auto oversized = host.startWorkerRequest();
    oversized.setWorkerId("oversizedbundle");
    oversized.setServices(services);
    oversized.setWorkerSource(
        kj::heapArray<kj::byte>(16 * 1024 * 1024 + 1).asPtr());
    oversized.send().wait(waitScope);
  });

  return 0;
}
