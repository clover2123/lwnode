/*
 * Copyright (c) 2021-present Samsung Electronics Co., Ltd
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "lwnode/lwnode.h"
#include <EscargotPublic.h>
#include <malloc.h>  // for malloc_trim
#include <chrono>
#include <codecvt>
#include <fstream>
#include "api.h"
#include "api/context.h"
#include "api/es-helper.h"
#include "api/isolate.h"
#include "api/utils/misc.h"
#include "api/utils/smaps.h"
#include "base.h"
#include "lwnode/lwnode-loader.h"

using namespace v8;
using namespace EscargotShim;
using namespace std::literals;

namespace LWNode {

static void SetMethod(ContextRef* context,
                      ObjectRef* target,
                      std::string name,
                      NativeFunctionPointer nativeFunction) {
  Evaluator::execute(
      context,
      [](ExecutionStateRef* state,
         ObjectRef* target,
         StringRef* name,
         NativeFunctionPointer nativeFunction) -> ValueRef* {
        target->defineDataProperty(
            state,
            name,
            FunctionObjectRef::create(state,
                                      FunctionObjectRef::NativeFunctionInfo(
                                          AtomicStringRef::emptyAtomicString(),
                                          nativeFunction,
                                          0,
                                          true,
                                          false)),
            true,
            true,
            true);

        return ValueRef::createUndefined();
      },
      target,
      StringRef::createFromUTF8(name.c_str(), name.length()),
      nativeFunction);
}

constexpr auto kSmapCacheDuration = 600ms;

static std::vector<SmapContents>& getSelfSmaps() {
  static std::vector<SmapContents> s_cachedSmaps = parseSmaps("self");
  static auto s_lastUpdatedTime = std::chrono::steady_clock::now();

  if (std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - s_lastUpdatedTime) <
      kSmapCacheDuration) {
    return s_cachedSmaps;
  }

  s_cachedSmaps = parseSmaps("self");
  s_lastUpdatedTime = std::chrono::steady_clock::now();
  return s_cachedSmaps;
}

static std::string createDumpFilePath() {
  std::string appName;
  std::ifstream("/proc/self/comm") >> appName;
  std::string outputPath =
      "/tmp/smaps-" + appName + "-" + getCurrentTimeString() + ".csv";
  return outputPath;
}

bool dumpSelfMemorySnapshot() {
  auto& smaps = getSelfSmaps();
  return dumpMemorySnapshot(createDumpFilePath(), smaps);
}

static ValueRef* PssUsage(ExecutionStateRef* state,
                          ValueRef* thisValue,
                          size_t argc,
                          ValueRef** argv,
                          bool isConstructCall) {
  auto& smaps = getSelfSmaps();
  size_t total = calculateTotal(smaps, kPss);
  return ValueRef::create(total);
};

static ValueRef* PssSwapUsage(ExecutionStateRef* state,
                              ValueRef* thisValue,
                              size_t argc,
                              ValueRef** argv,
                              bool isConstructCall) {
  auto& smaps = getSelfSmaps();
  size_t total = calculateTotalPssSwap(smaps);
  return ValueRef::create(total);
};

static ValueRef* RssUsage(ExecutionStateRef* state,
                          ValueRef* thisValue,
                          size_t argc,
                          ValueRef** argv,
                          bool isConstructCall) {
  auto& smaps = getSelfSmaps();
  size_t total = calculateTotalRss(smaps);
  return ValueRef::create(total);
};

static ValueRef* MemSnapshot(ExecutionStateRef* state,
                             ValueRef* thisValue,
                             size_t argc,
                             ValueRef** argv,
                             bool isConstructCall) {
#ifdef PRODUCTION
  return ValueRef::create(false);
#endif
  auto outputPath = createDumpFilePath();
  auto& smaps = getSelfSmaps();
  if (dumpMemorySnapshot(outputPath, smaps)) {
    return StringRef::createFromUTF8(outputPath.c_str(), outputPath.length());
  }
  return ValueRef::createUndefined();
};

static ValueRef* CreateReloadableSourceFromFile(ExecutionStateRef* state,
                                                ValueRef* thisValue,
                                                size_t argc,
                                                ValueRef** argv,
                                                bool isConstructCall) {
  if (argc > 0) {
    if (argv[0]->isString()) {
      std::string fileName = argv[0]
                                 ->toStringWithoutException(state->context())
                                 ->toStdUTF8String();
      return Loader::CreateReloadableSourceFromFile(state, fileName);
    }
  }
  return ValueRef::createUndefined();
}

void InitializeProcessMethods(Local<Object> target, Local<Context> context) {
  auto esContext = CVAL(*context)->context()->get();
  auto esTarget = CVAL(*target)->value()->asObject();

#if !defined(NDEBUG)
  EvalResultHelper::attachBuiltinPrint(esContext, esTarget);
#endif

  SetMethod(esContext, esTarget, "PssUsage", PssUsage);
  SetMethod(esContext, esTarget, "RssUsage", RssUsage);
  SetMethod(esContext, esTarget, "PssSwapUsage", PssSwapUsage);
  SetMethod(esContext, esTarget, "MemSnapshot", MemSnapshot);
#ifdef LWNODE_USE_RELOAD_SCRIPT
  SetMethod(esContext,
            esTarget,
            "CreateReloadableSourceFromFile",
            CreateReloadableSourceFromFile);
#endif
}

static void IdleGC(v8::Isolate* isolate) {
  LWNODE_LOG_INFO("IdleGC");
  IsolateWrap::fromV8(isolate)->vmInstance()->enterIdleMode();
  Escargot::Memory::gc();
  malloc_trim(0);
}

void MessageLoop::OnPrepare(v8::Isolate* isolate) {
  // @note escargot says:
  // user can call 'enterIdleMode' many times without performance concern

  // @todo consider delayed-entering Idle GC
  IdleGC(isolate);
}

}  // namespace LWNode