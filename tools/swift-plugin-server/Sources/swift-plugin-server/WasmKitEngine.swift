//===----------------------------------------------------------------------===//
//
// This source file is part of the Swift open source project
//
// Copyright (c) 2024 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#if !SWIFT_WASM_USE_JSC

import WASI
import WasmKit
import WasmKitWASI
import SystemPackage

typealias DefaultWasmEngine = WasmKitEngine

struct WasmKitEngine: WasmEngine {
  private let module: Module
  private let instance: ModuleInstance
  private let runtime: Runtime

  init(path: FilePath, imports: WASIBridgeToHost) throws {
    // we never call wasm.deallocator, effectively leaking the data,
    // but that's intentional because plugins can't be "unloaded"
    module = try parseWasm(filePath: path)
    runtime = Runtime(hostModules: imports.hostModules)
    instance = try runtime.instantiate(module: module)
  }

  func function(named name: String) throws -> WasmFunction? {
    guard case let .function(function) = instance.exportInstances.first(where: { $0.name == name })?.value else {
      return nil
    }
    return { args in try function.invoke(args.map(Value.i32), runtime: runtime).map(\.i32) }
  }
}

#endif
