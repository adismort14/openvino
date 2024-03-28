// Copyright (C) 2018-2024 Intel Corporation
// SPDX-License-Identifier: Apache-2.0

#include "node/include/core_wrap.hpp"

#include "node/include/addon.hpp"
#include "node/include/async_reader.hpp"
#include "node/include/compiled_model.hpp"
#include "node/include/errors.hpp"
#include "node/include/helper.hpp"
#include "node/include/model_wrap.hpp"
#include "node/include/read_model_args.hpp"

CoreWrap::CoreWrap(const Napi::CallbackInfo& info) : Napi::ObjectWrap<CoreWrap>(info), _core{} {}

Napi::Function CoreWrap::get_class(Napi::Env env) {
    return DefineClass(env,
                       "Core",
                       {InstanceMethod("readModelSync", &CoreWrap::read_model_sync),
                        InstanceMethod("readModel", &CoreWrap::read_model_async),
                        InstanceMethod("compileModelSync", &CoreWrap::compile_model_sync_dispatch),
                        InstanceMethod("compileModel", &CoreWrap::compile_model_async),
                        InstanceMethod("importModelSync", &CoreWrap::import_model),
                        InstanceMethod("getAvailableDevices", &CoreWrap::get_available_devices),
                        InstanceMethod("getVersions", &CoreWrap::get_versions)});
}

Napi::Value CoreWrap::read_model_sync(const Napi::CallbackInfo& info) {
    try {
        ReadModelArgs* args;
        args = new ReadModelArgs(info);
        auto model = args->model_str.empty() ? _core.read_model(args->model_path, args->bin_path)
                                             : _core.read_model(args->model_str, args->weight_tensor);
        delete args;

        return ModelWrap::wrap(info.Env(), model);
    } catch (std::runtime_error& err) {
        reportError(info.Env(), err.what());

        return info.Env().Undefined();
    }
}

Napi::Value CoreWrap::read_model_async(const Napi::CallbackInfo& info) {
    try {
        ReadModelArgs* args = new ReadModelArgs(info);
        ReaderWorker* _readerWorker = new ReaderWorker(info.Env(), args);
        _readerWorker->Queue();

        return _readerWorker->GetPromise();
    } catch (std::runtime_error& err) {
        reportError(info.Env(), err.what());

        return info.Env().Undefined();
    }
}

Napi::Value CoreWrap::compile_model_sync(const Napi::CallbackInfo& info,
                                         const Napi::Object& model,
                                         const Napi::String& device) {
    const auto& model_prototype = info.Env().GetInstanceData<AddonData>()->model;
    if (model_prototype && model.InstanceOf(model_prototype.Value().As<Napi::Function>())) {
        const auto m = Napi::ObjectWrap<ModelWrap>::Unwrap(model);
        const auto& compiled_model = _core.compile_model(m->get_model(), device);
        return CompiledModelWrap::wrap(info.Env(), compiled_model);
    } else {
        reportError(info.Env(), "Cannot create Model from Napi::Object.");
        return info.Env().Undefined();
    }
}

Napi::Value CoreWrap::compile_model_sync(const Napi::CallbackInfo& info,
                                         const Napi::String& model_path,
                                         const Napi::String& device) {
    const auto& compiled_model = _core.compile_model(model_path, device);
    return CompiledModelWrap::wrap(info.Env(), compiled_model);
}

Napi::Value CoreWrap::compile_model_sync(const Napi::CallbackInfo& info,
                                         const Napi::Object& model_obj,
                                         const Napi::String& device,
                                         const std::map<std::string, ov::Any>& config) {
    const auto& mw = Napi::ObjectWrap<ModelWrap>::Unwrap(model_obj);
    const auto& compiled_model = _core.compile_model(mw->get_model(), info[1].ToString(), config);
    return CompiledModelWrap::wrap(info.Env(), compiled_model);
}

Napi::Value CoreWrap::compile_model_sync(const Napi::CallbackInfo& info,
                                         const Napi::String& model_path,
                                         const Napi::String& device,
                                         const std::map<std::string, ov::Any>& config) {
    const auto& compiled_model = _core.compile_model(model_path, device, config);
    return CompiledModelWrap::wrap(info.Env(), compiled_model);
}

Napi::Value CoreWrap::compile_model_sync_dispatch(const Napi::CallbackInfo& info) {
    try {
        if (info.Length() == 2 && info[0].IsString() && info[1].IsString()) {
            return compile_model_sync(info, info[0].ToString(), info[1].ToString());
        } else if (info.Length() == 2 && info[0].IsObject() && info[1].IsString()) {
            return compile_model_sync(info, info[0].ToObject(), info[1].ToString());
        } else if (info.Length() == 3 && info[0].IsString() && info[1].IsString()) {
            const auto& config = js_to_cpp<std::map<std::string, ov::Any>>(info, 2, {napi_object});
            return compile_model_sync(info, info[0].ToString(), info[1].ToString(), config);
        } else if (info.Length() == 3 && info[0].IsObject() && info[1].IsString()) {
            const auto& config = js_to_cpp<std::map<std::string, ov::Any>>(info, 2, {napi_object});
            return compile_model_sync(info, info[0].ToObject(), info[1].ToString(), config);
        } else if (info.Length() < 2 || info.Length() > 3) {
            reportError(info.Env(), "Invalid number of arguments -> " + std::to_string(info.Length()));
            return info.Env().Undefined();
        } else {
            reportError(info.Env(), "Error while compiling model.");
            return info.Env().Undefined();
        }
    } catch (std::exception& e) {
        reportError(info.Env(), e.what());
        return info.Env().Undefined();
    }
}

void FinalizerCallbackModel(Napi::Env env, void* finalizeData, TsfnContextModel* context) {
    context->nativeThread.join();
    delete context;
};

void FinalizerCallbackPath(Napi::Env env, void* finalizeData, TsfnContextPath* context) {
    context->nativeThread.join();
    delete context;
};

void compileModelThreadModel(TsfnContextModel* context) {
    ov::Core core;
    context->_compiled_model = core.compile_model(context->_model, context->_device, context->_config);

    auto callback = [](Napi::Env env, Napi::Function, TsfnContextModel* context) {
        Napi::HandleScope scope(env);
        auto obj = CompiledModelWrap::get_class(env).New({});
        auto cm = Napi::ObjectWrap<CompiledModelWrap>::Unwrap(obj);
        cm->set_compiled_model(context->_compiled_model);

        context->deferred.Resolve(obj);
    };

    context->tsfn.BlockingCall(context, callback);
    context->tsfn.Release();
}

void compileModelThreadPath(TsfnContextPath* context) {
    ov::Core core;
    context->_compiled_model = core.compile_model(context->_model, context->_device, context->_config);

    auto callback = [](Napi::Env env, Napi::Function, TsfnContextPath* context) {
        Napi::HandleScope scope(env);
        auto obj = CompiledModelWrap::get_class(env).New({});
        auto cm = Napi::ObjectWrap<CompiledModelWrap>::Unwrap(obj);
        cm->set_compiled_model(context->_compiled_model);

        context->deferred.Resolve(obj);
    };

    context->tsfn.BlockingCall(context, callback);
    context->tsfn.Release();
}

Napi::Value CoreWrap::compile_model_async(const Napi::CallbackInfo& info) {
    auto env = info.Env();
    if (info[0].IsObject() && info[1].IsString()) {
        auto context_data = new TsfnContextModel(env);
        auto m = Napi::ObjectWrap<ModelWrap>::Unwrap(info[0].ToObject());
        context_data->_model = m->get_model()->clone();
        context_data->_device = info[1].ToString();

        if (info.Length() == 3) {
            try {
                context_data->_config = js_to_cpp<std::map<std::string, ov::Any>>(info, 2, {napi_object});
            } catch (std::exception& e) {
                reportError(env, e.what());
            }
        }

        context_data->tsfn = Napi::ThreadSafeFunction::New(env,
                                                           Napi::Function(),
                                                           "TSFN",
                                                           0,
                                                           1,
                                                           context_data,
                                                           FinalizerCallbackModel,
                                                           (void*)nullptr);

        context_data->nativeThread = std::thread(compileModelThreadModel, context_data);
        return context_data->deferred.Promise();
    } else if (info[0].IsString() && info[1].IsString()) {
        auto context_data = new TsfnContextPath(env);
        context_data->_model = info[0].ToString();
        context_data->_device = info[1].ToString();

        if (info.Length() == 3) {
            try {
                context_data->_config = js_to_cpp<std::map<std::string, ov::Any>>(info, 2, {napi_object});
            } catch (std::exception& e) {
                reportError(env, e.what());
            }
        }

        context_data->tsfn = Napi::ThreadSafeFunction::New(env,
                                                           Napi::Function(),
                                                           "TSFN",
                                                           0,
                                                           1,
                                                           context_data,
                                                           FinalizerCallbackPath,
                                                           (void*)nullptr);

        context_data->nativeThread = std::thread(compileModelThreadPath, context_data);
        return context_data->deferred.Promise();
    } else if (info.Length() < 2 || info.Length() > 3) {
        reportError(info.Env(), "Invalid number of arguments -> " + std::to_string(info.Length()));
        return Napi::Value();
    } else {
        reportError(info.Env(), "Error while compiling model.");
        return Napi::Value();
    }
}

Napi::Value CoreWrap::get_available_devices(const Napi::CallbackInfo& info) {
    const auto& devices = _core.get_available_devices();
    Napi::Array js_devices = Napi::Array::New(info.Env(), devices.size());

    uint32_t i = 0;
    for (const auto& dev : devices)
        js_devices[i++] = dev;

    return js_devices;
}

Napi::Value CoreWrap::get_versions(const Napi::CallbackInfo& info) {
    if (info.Length() == 0) {
        reportError(info.Env(), "No argument provided in the getVersions() method call.");
        return info.Env().Undefined();
    }
    auto device_arg = info[0];
    if (!device_arg.IsString()) {
        reportError(info.Env(), "The argument in getVersions() method must be a string or convertible to a string.");
        return info.Env().Undefined();
    }
    const auto& devices_map = _core.get_versions(device_arg.ToString());
    Napi::Object versionsObject = Napi::Object::New(info.Env());

    for (const auto& dev : devices_map) {
        Napi::Object deviceProperties = Napi::Object::New(info.Env());

        deviceProperties.Set("buildNumber", Napi::String::New(info.Env(), dev.second.buildNumber));
        deviceProperties.Set("description", Napi::String::New(info.Env(), dev.second.description));

        versionsObject.Set(dev.first, deviceProperties);
    }

    return versionsObject;
}

Napi::Value CoreWrap::import_model(const Napi::CallbackInfo& info) {
    if (info.Length() != 2) {
        reportError(info.Env(), "Invalid number of arguments -> " + std::to_string(info.Length()));
        return info.Env().Undefined();
    }
    if (!info[0].IsBuffer()) {
        reportError(info.Env(), "The first argument must be of type Buffer.");
        return info.Env().Undefined();
    }
    if (!info[1].IsString()) {
        reportError(info.Env(), "The second argument must be of type String.");
        return info.Env().Undefined();
    }
    const auto& model_data = info[0].As<Napi::Buffer<uint8_t>>();
    const auto model_stream = std::string(reinterpret_cast<char*>(model_data.Data()), model_data.Length());
    std::stringstream _stream;
    _stream << model_stream;

    const auto& compiled = _core.import_model(_stream, std::string(info[1].ToString()));
    return CompiledModelWrap::wrap(info.Env(), compiled);
}
