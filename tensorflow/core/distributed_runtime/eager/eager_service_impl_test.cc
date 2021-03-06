/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/distributed_runtime/eager/eager_service_impl.h"

#include <string.h>

#include <memory>

#include "absl/types/optional.h"
#include "tensorflow/c/c_api_internal.h"
#include "tensorflow/core/common_runtime/eager/kernel_and_device.h"
#include "tensorflow/core/common_runtime/eager/process_function_library_runtime.h"
#include "tensorflow/core/common_runtime/eager/tensor_handle.h"
#include "tensorflow/core/distributed_runtime/eager/cluster_function_library_runtime.h"
#include "tensorflow/core/distributed_runtime/eager/remote_mgr.h"
#include "tensorflow/core/distributed_runtime/rpc/rpc_rendezvous_mgr.h"
#include "tensorflow/core/distributed_runtime/session_mgr.h"
#include "tensorflow/core/distributed_runtime/test_utils.h"
#include "tensorflow/core/distributed_runtime/worker_env.h"
#include "tensorflow/core/framework/attr_value.pb.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/lib/random/random.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/macros.h"
#include "tensorflow/core/platform/protobuf.h"
#include "tensorflow/core/platform/test.h"
#include "tensorflow/core/protobuf/eager_service.pb.h"
#include "tensorflow/core/protobuf/remote_tensor_handle.pb.h"
#include "tensorflow/core/protobuf/tensorflow_server.pb.h"

namespace tensorflow {
namespace eager {
namespace {

class TestEagerServiceImpl : public EagerServiceImpl {
 public:
  explicit TestEagerServiceImpl(const WorkerEnv* env) : EagerServiceImpl(env) {}
  Status GetEagerContext(const uint64 context_id, EagerContext** ctx) {
    ServerContext* context = nullptr;
    TF_RETURN_IF_ERROR(GetServerContext(context_id, &context));
    core::ScopedUnref context_unref(context);
    *ctx = context->Context();
    return Status::OK();
  }
  Status GetTensorHandle(const uint64 context_id,
                         const RemoteTensorHandleInternal& remote_handle,
                         tensorflow::TensorHandle** handle) {
    ServerContext* context = nullptr;
    TF_RETURN_IF_ERROR(GetServerContext(context_id, &context));
    core::ScopedUnref context_unref(context);

    return context->Context()->RemoteMgr()->GetTensorHandle(remote_handle,
                                                            handle);
  }
};

class FakeEagerClient : public EagerClient {
 public:
  FakeEagerClient() {}
  ~FakeEagerClient() override {}

  void SetServiceImpl(TestEagerServiceImpl* impl) { impl_ = impl; }

#define CLIENT_METHOD(method)                                         \
  void method##Async(const method##Request* request,                  \
                     method##Response* response, StatusCallback done) \
      override {                                                      \
    done(impl_->method(request, response));                           \
  }

  CLIENT_METHOD(CreateContext);
  CLIENT_METHOD(UpdateContext);
  CLIENT_METHOD(Enqueue);
  CLIENT_METHOD(WaitQueueDone);
  CLIENT_METHOD(KeepAlive);
  CLIENT_METHOD(CloseContext);
#undef CLIENT_METHOD

  void StreamingEnqueueAsync(const EnqueueRequest* request,
                             EnqueueResponse* response,
                             StatusCallback done) override {
    done(impl_->Enqueue(request, response));
  }

 private:
  TestEagerServiceImpl* impl_;
};

class DummyEagerClientCache : public EagerClientCache {
 public:
  DummyEagerClientCache() : client_(new FakeEagerClient) {}
  Status GetClient(const string& target, EagerClient** client) override {
    *client = client_.get();
    return Status::OK();
  }

 private:
  std::unique_ptr<EagerClient> client_;
};

class FakeCache : public TestWorkerCache {
  Status GetEagerClientCache(
      std::unique_ptr<eager::EagerClientCache>* eager_client_cache) override {
    eager_client_cache->reset(new DummyEagerClientCache);
    return Status::OK();
  }

  void ListWorkers(std::vector<string>* workers) const override {
    workers->push_back("/job:localhost/replica:0/task:0");
  }
};

class EagerServiceImplTest : public ::testing::Test {
 public:
  EagerServiceImplTest()
      : rendezvous_mgr_(&worker_env_),
        session_mgr_(new SessionMgr(
            &worker_env_, "/job:localhost/replica:0/task:0/device:CPU:0",
            std::unique_ptr<WorkerCacheInterface>(new FakeCache),
            [](const ServerDef& server_def,
               WorkerCacheInterface** worker_cache) {
              *worker_cache = new FakeCache;
              return Status::OK();
            })) {
    worker_env_.env = Env::Default();

    worker_env_.rendezvous_mgr = &rendezvous_mgr_;
    worker_env_.session_mgr = session_mgr_.get();

    device_mgr_ = absl::make_unique<StaticDeviceMgr>(
        DeviceFactory::NewDevice("CPU", {}, "/job:localhost/replica:0/task:0"));
    worker_env_.local_devices = device_mgr_->ListDevices();
    worker_env_.device_mgr = device_mgr_.get();
  }

 protected:
  WorkerEnv worker_env_;
  tensorflow::RpcRendezvousMgr rendezvous_mgr_;
  std::unique_ptr<SessionMgr> session_mgr_;
  std::unique_ptr<DeviceMgr> device_mgr_;
};

void SetTensorProto(TensorProto* tensor_proto) {
  int64_t dims[] = {2, 2};
  float data[] = {1.0f, 2.0f, 3.0f, 4.0f};
  TF_Tensor* t = TF_AllocateTensor(
      TF_FLOAT, &dims[0], sizeof(dims) / sizeof(int64_t), sizeof(data));
  memcpy(TF_TensorData(t), &data[0], TF_TensorByteSize(t));
  tensorflow::Tensor tensor;
  TF_ASSERT_OK(tensorflow::TF_TensorToTensor(t, &tensor));
  tensor.AsProtoTensorContent(tensor_proto);
  TF_DeleteTensor(t);
}

void AddOperationToEnqueueRequest(
    int64 id, const string& name,
    const std::vector<std::pair<int64, int32>>& inputs,
    const std::unordered_map<string, AttrValue>& attrs, const string& device,
    EnqueueRequest* request) {
  auto* operation = request->add_queue()->mutable_operation();

  operation->set_id(id);
  operation->set_name(name);
  operation->set_device(device);

  for (const auto& tensor_handle_pair : inputs) {
    auto* input = operation->add_inputs();
    input->set_op_id(tensor_handle_pair.first);
    input->set_output_num(tensor_handle_pair.second);
    input->set_op_device(device);
    input->set_device(device);
  }

  for (const auto& attr_entry : attrs) {
    (*operation->mutable_attrs())[attr_entry.first] = attr_entry.second;
  }
}

tensorflow::NodeDef MatMulFunctionNodeDef() {
  tensorflow::NodeDef def;
  CHECK(tensorflow::protobuf::TextFormat::ParseFromString(
      "    name: 'matmul_func'"
      "    op: 'MatMulFunction'"
      "    input: 'a'"
      "    input: 'a'"
      "    attr {"
      "      key: 'T'"
      "      value {"
      "        type: DT_FLOAT"
      "      }"
      "    }",
      &def));
  return def;
}

tensorflow::FunctionDef MatMulFunction() {
  tensorflow::FunctionDef def;
  CHECK(tensorflow::protobuf::TextFormat::ParseFromString(
      "    signature {"
      "      name: 'MatMulFunction'"
      "      input_arg {"
      "        name: 'a'"
      "        type: DT_FLOAT"
      "      }"
      "      output_arg {"
      "        name: 'm'"
      "        type: DT_FLOAT"
      "      }"
      "    }"
      "    node_def {"
      "      name: 'matmul'"
      "      op: 'MatMul'"
      "      input: 'a'"
      "      input: 'a'"
      "      attr {"
      "        key: 'T'"
      "        value {"
      "          type: DT_FLOAT"
      "        }"
      "      }"
      "    }"
      "    ret {"
      "      key: 'm'"
      "      value: 'matmul:product'"
      "    }",
      &def));
  return def;
}

// Test creates a context and attempts to execute some ops.
TEST_F(EagerServiceImplTest, BasicTest) {
  TestEagerServiceImpl eager_service_impl(&worker_env_);

  uint64 context_id = random::New64();

  CreateContextRequest request;
  request.mutable_server_def()->set_job_name("localhost");
  request.mutable_server_def()->set_task_index(0);
  request.set_context_id(context_id);
  CreateContextResponse response;

  TF_ASSERT_OK(eager_service_impl.CreateContext(&request, &response));

  EnqueueRequest remote_enqueue_request;
  remote_enqueue_request.set_context_id(context_id);
  EnqueueResponse remote_enqueue_response;

  std::unordered_map<string, AttrValue> const_attrs;
  AttrValue val;
  val.set_type(tensorflow::DataType::DT_FLOAT);
  const_attrs.insert({"dtype", val});
  val.Clear();
  SetTensorProto(val.mutable_tensor());
  const_attrs.insert({"value", val});

  AddOperationToEnqueueRequest(1, "Const", {}, const_attrs,
                               "/job:localhost/replica:0/task:0/device:CPU:0",
                               &remote_enqueue_request);

  std::unordered_map<string, AttrValue> attrs;
  val.Clear();
  val.set_type(tensorflow::DataType::DT_FLOAT);
  attrs.insert({"T", val});
  val.Clear();
  val.set_b(false);
  attrs.insert({"transpose_a", val});
  attrs.insert({"transpose_b", val});

  AddOperationToEnqueueRequest(2, "MatMul", {{1, 0}, {1, 0}}, attrs,
                               "/job:localhost/replica:0/task:0/device:CPU:0",
                               &remote_enqueue_request);

  TF_ASSERT_OK(eager_service_impl.Enqueue(&remote_enqueue_request,
                                          &remote_enqueue_response));

  auto& matmul_result_shape =
      remote_enqueue_response.queue_response(1).shape(0);
  EXPECT_EQ(matmul_result_shape.dim(0).size(), 2);
  EXPECT_EQ(matmul_result_shape.dim(1).size(), 2);

  tensorflow::TensorHandle* tensor_handle;
  TF_ASSERT_OK(eager_service_impl.GetTensorHandle(
      context_id, RemoteTensorHandleInternal(2, 0), &tensor_handle));

  // This should be OK to do since we've placed all computation on the CPU
  // device.
  const tensorflow::Tensor* t = nullptr;
  TF_ASSERT_OK(tensor_handle->Tensor(&t));

  auto actual = t->flat<float>();

  EXPECT_EQ(4, actual.size());

  EXPECT_EQ(7, actual(0));
  EXPECT_EQ(10, actual(1));
  EXPECT_EQ(15, actual(2));
  EXPECT_EQ(22, actual(3));

  CloseContextRequest close_context_request;
  close_context_request.set_context_id(context_id);
  CloseContextResponse close_context_response;
  TF_ASSERT_OK(eager_service_impl.CloseContext(&close_context_request,
                                               &close_context_response));
}

// Test creates a context and attempts to execute a function.
TEST_F(EagerServiceImplTest, BasicFunctionTest) {
  TestEagerServiceImpl eager_service_impl(&worker_env_);

  uint64 context_id = random::New64();

  CreateContextRequest request;
  request.mutable_server_def()->set_job_name("localhost");
  request.mutable_server_def()->set_task_index(0);
  request.set_context_id(context_id);
  CreateContextResponse response;

  TF_ASSERT_OK(eager_service_impl.CreateContext(&request, &response));

  EnqueueRequest enqueue_request;
  enqueue_request.set_context_id(context_id);
  RegisterFunctionOp* register_function =
      enqueue_request.add_queue()->mutable_register_function();
  *register_function->mutable_function_def() = MatMulFunction();
  EnqueueResponse enqueue_response;

  TF_ASSERT_OK(eager_service_impl.Enqueue(&enqueue_request, &enqueue_response));

  EnqueueRequest remote_enqueue_request;
  remote_enqueue_request.set_context_id(context_id);
  EnqueueResponse remote_enqueue_response;

  std::unordered_map<string, AttrValue> const_attrs;
  AttrValue val;
  val.set_type(tensorflow::DataType::DT_FLOAT);
  const_attrs.insert({"dtype", val});
  val.Clear();

  SetTensorProto(val.mutable_tensor());
  const_attrs.insert({"value", val});

  AddOperationToEnqueueRequest(1, "Const", {}, const_attrs,
                               "/job:localhost/replica:0/task:0/device:CPU:0",
                               &remote_enqueue_request);
  AddOperationToEnqueueRequest(
      2, "MatMulFunction", {{1, 0}}, std::unordered_map<string, AttrValue>(),
      "/job:localhost/replica:0/task:0/device:CPU:0", &remote_enqueue_request);

  TF_ASSERT_OK(eager_service_impl.Enqueue(&remote_enqueue_request,
                                          &remote_enqueue_response));

  const tensorflow::Tensor* t = nullptr;
  tensorflow::TensorHandle* tensor_handle;
  TF_ASSERT_OK(eager_service_impl.GetTensorHandle(
      context_id, RemoteTensorHandleInternal(2, 0), &tensor_handle));
  TF_ASSERT_OK(tensor_handle->Tensor(&t));

  auto actual = t->flat<float>();
  EXPECT_EQ(4, actual.size());

  EXPECT_EQ(7, actual(0));
  EXPECT_EQ(10, actual(1));
  EXPECT_EQ(15, actual(2));
  EXPECT_EQ(22, actual(3));

  CloseContextRequest close_context_request;
  close_context_request.set_context_id(context_id);
  CloseContextResponse close_context_response;
  TF_ASSERT_OK(eager_service_impl.CloseContext(&close_context_request,
                                               &close_context_response));
}

class FunctionWithRemoteInputsTest : public EagerServiceImplTest {
 public:
  FunctionWithRemoteInputsTest()
      : EagerServiceImplTest(), eager_service_impl_(&worker_env_) {
    remote_device_mgr_ = absl::make_unique<StaticDeviceMgr>(
        DeviceFactory::NewDevice("CPU", {}, "/job:localhost/replica:0/task:1"));
    context_id_ = random::New64();
  }

  class TestExecuteNodeArgs : public EagerKernelArgs {
   public:
    TestExecuteNodeArgs(
        gtl::InlinedVector<TensorValue, 4>&& tensor_args,
        std::function<Status(const int, eager::RemoteTensorHandle*)>
            serialize_remote_handle)
        : EagerKernelArgs(std::move(tensor_args)),
          serialize_remote_handle_(std::move(serialize_remote_handle)) {}

    bool HasRemoteInputs() const override { return true; }

    Status GetRemoteArg(const int index,
                        eager::RemoteTensorHandle* val) const override {
      return serialize_remote_handle_(index, val);
    }

   private:
    std::function<Status(const int, eager::RemoteTensorHandle*)>
        serialize_remote_handle_;
  };

  void Init() {
    CreateContextRequest request;
    request.mutable_server_def()->set_job_name("localhost");
    request.mutable_server_def()->set_task_index(0);
    request.set_context_id(context_id_);
    CreateContextResponse response;
    TF_ASSERT_OK(eager_service_impl_.CreateContext(&request, &response));

    // Make the fake EagerClient use the local eager_service_impl.
    EagerContext* ctx = nullptr;
    TF_ASSERT_OK(eager_service_impl_.GetEagerContext(context_id_, &ctx));
    Device* device;
    TF_ASSERT_OK(ctx->FindDeviceFromName(local_device_.c_str(), &device));
    EagerClient* client;
    TF_ASSERT_OK(ctx->GetClient(device, &client));
    FakeEagerClient* fake_client = static_cast<FakeEagerClient*>(client);
    fake_client->SetServiceImpl(&eager_service_impl_);

    // Create an input on local_device for MatMulFunction.
    EnqueueRequest remote_enqueue_request;
    remote_enqueue_request.set_context_id(context_id_);
    EnqueueResponse remote_enqueue_response;
    std::unordered_map<string, AttrValue> const_attrs;
    AttrValue val;
    val.set_type(tensorflow::DataType::DT_FLOAT);
    const_attrs.insert({"dtype", val});
    val.Clear();
    SetTensorProto(val.mutable_tensor());
    const_attrs.insert({"value", val});
    AddOperationToEnqueueRequest(1, "Const", {}, const_attrs, local_device_,
                                 &remote_enqueue_request);
    TF_EXPECT_OK(eager_service_impl_.Enqueue(&remote_enqueue_request,
                                             &remote_enqueue_response));
    eager_cluster_flr_ = absl::make_unique<EagerClusterFunctionLibraryRuntime>(
        ctx, device_mgr_.get());

    fdef_ = MatMulFunction();
    TF_ASSERT_OK(func_lib_def_.AddFunctionDef(fdef_));
    eager_pflr_ = absl::make_unique<EagerProcessFunctionLibraryRuntime>(
        remote_device_mgr_.get(), Env::Default(), /*config=*/nullptr,
        TF_GRAPH_DEF_VERSION, &func_lib_def_, OptimizerOptions(), nullptr,
        eager_cluster_flr_.get(), nullptr);
  }

  void CheckOutputsAndClose(const int64 op_id) {
    const tensorflow::Tensor* t = nullptr;
    tensorflow::TensorHandle* tensor_handle;
    TF_ASSERT_OK(eager_service_impl_.GetTensorHandle(
        context_id_, RemoteTensorHandleInternal(2, 0), &tensor_handle));
    TF_ASSERT_OK(tensor_handle->Tensor(&t));
    auto actual = t->flat<float>();
    EXPECT_EQ(4, actual.size());
    EXPECT_EQ(7, actual(0));
    EXPECT_EQ(10, actual(1));
    EXPECT_EQ(15, actual(2));
    EXPECT_EQ(22, actual(3));

    CloseContextRequest close_context_request;
    close_context_request.set_context_id(context_id_);
    CloseContextResponse close_context_response;
    TF_ASSERT_OK(eager_service_impl_.CloseContext(&close_context_request,
                                                  &close_context_response));
  }

 protected:
  const string local_device_ = "/job:localhost/replica:0/task:0/device:CPU:0";
  const string remote_device_ = "/job:localhost/replica:0/task:1/device:CPU:0";
  TestEagerServiceImpl eager_service_impl_;
  std::unique_ptr<DeviceMgr> remote_device_mgr_;
  uint64 context_id_;
  tensorflow::FunctionDef fdef_;
  std::unique_ptr<ProcessFunctionLibraryRuntime> eager_pflr_;

 private:
  FunctionLibraryDefinition func_lib_def_{OpRegistry::Global(), {}};
  std::unique_ptr<EagerClusterFunctionLibraryRuntime> eager_cluster_flr_;
};

// Test executes a remote function through
// EagerProcessFunctionLibraryRuntime(EagerClusterFunctionLibraryRuntime).
TEST_F(FunctionWithRemoteInputsTest, EagerPFLRTest) {
  Init();
  // Instantiate MatMulFunction on remote_device.
  FunctionLibraryRuntime::InstantiateOptions options;
  options.target = remote_device_;
  options.is_multi_device_function = true;
  options.input_devices.push_back(local_device_);
  FunctionLibraryRuntime::Handle handle;
  TF_ASSERT_OK(eager_pflr_->Instantiate(
      fdef_.signature().name(), AttrSlice(&fdef_.attr()), options, &handle));
  bool is_cross_process = false;
  TF_CHECK_OK(eager_pflr_->IsCrossProcess(handle, &is_cross_process));
  EXPECT_TRUE(is_cross_process);

  // Run MatMulFunction on remote_device.
  FunctionLibraryRuntime::Options opts;
  const uint64 op_id = 2;
  opts.op_id = op_id;
  Notification done;
  Status status;
  RemoteTensorHandle input;
  input.set_op_id(1);
  input.set_output_num(0);
  input.set_op_device(local_device_);
  input.set_device(local_device_);
  std::vector<RemoteTensorHandle> inputs = {input};
  std::vector<Tensor> outputs;
  gtl::InlinedVector<TensorValue, 4> tensor_args = {TensorValue()};
  TestExecuteNodeArgs args(
      std::move(tensor_args),
      [&inputs](const int i, RemoteTensorHandle* handle) -> Status {
        *handle = inputs.at(i);
        return Status::OK();
      });
  eager_pflr_->Run(opts, handle, args, &outputs,
                   [&status, &done](const Status& s) {
                     status = s;
                     done.Notify();
                   });
  done.WaitForNotification();
  TF_ASSERT_OK(status);
  CheckOutputsAndClose(op_id);
}

// Test executes a remote function through KernelAndDeviceFunc.
TEST_F(FunctionWithRemoteInputsTest, KernelAndDeviceFuncTest) {
  Init();
  Device* local_device;
  TF_ASSERT_OK(device_mgr_->LookupDevice(local_device_, &local_device));
  std::vector<Device*> input_dev_ptrs;
  input_dev_ptrs.push_back(local_device);
  FunctionLibraryRuntime* flr = eager_pflr_->GetFLR(remote_device_);
  EagerContext* ctx = nullptr;
  TF_ASSERT_OK(eager_service_impl_.GetEagerContext(context_id_, &ctx));
  core::RefCountPtr<KernelAndDeviceFunc> kernel = nullptr;
  const int64 op_id = 2;
  kernel.reset(new KernelAndDeviceFunc(
      flr, eager_pflr_.get(), std::move(input_dev_ptrs), {}, nullptr, nullptr,
      local_device, fdef_.signature().name(),
      [ctx](const int64 step_id) { return ctx->CreateRendezvous(step_id); },
      []() { return op_id; }));

  // Instantiate MatMulFunction on remote_device.
  const NodeDef node_def = MatMulFunctionNodeDef();
  TF_ASSERT_OK(kernel->InstantiateFunc(node_def, nullptr));

  // Run MatMulFunction on remote_device.
  gtl::InlinedVector<TensorValue, 4> input_tensors = {TensorValue()};
  RemoteTensorHandle input;
  input.set_op_id(1);
  input.set_output_num(0);
  input.set_op_device(local_device_);
  input.set_device(local_device_);
  std::vector<RemoteTensorHandle> remote_handles = {input};
  TestExecuteNodeArgs inputs(
      std::move(input_tensors),
      [&remote_handles](const int index, RemoteTensorHandle* handle) -> Status {
        *handle = remote_handles.at(index);
        return Status::OK();
      });
  std::vector<Tensor> outputs;

  TF_ASSERT_OK(kernel->Run(inputs, &outputs, nullptr, absl::nullopt));

  CheckOutputsAndClose(op_id);
}

// Test creates a context and attempts to send a tensor (using the RPC), and
// then use the tensor.
TEST_F(EagerServiceImplTest, SendTensorTest) {
  TestEagerServiceImpl eager_service_impl(&worker_env_);

  uint64 context_id = random::New64();

  CreateContextRequest request;
  request.mutable_server_def()->set_job_name("localhost");
  request.mutable_server_def()->set_task_index(0);
  request.set_context_id(context_id);
  CreateContextResponse response;

  TF_ASSERT_OK(eager_service_impl.CreateContext(&request, &response));

  EnqueueRequest remote_enqueue_request;
  remote_enqueue_request.set_context_id(context_id);
  EnqueueResponse remote_enqueue_response;

  auto* send_tensor = remote_enqueue_request.add_queue()->mutable_send_tensor();
  send_tensor->set_op_id(1);
  SetTensorProto(send_tensor->add_tensors());

  std::unordered_map<string, AttrValue> attrs;
  AttrValue val;
  val.Clear();
  val.set_type(tensorflow::DataType::DT_FLOAT);
  attrs.insert({"T", val});
  val.Clear();
  val.set_b(false);
  attrs.insert({"transpose_a", val});
  attrs.insert({"transpose_b", val});

  AddOperationToEnqueueRequest(2, "MatMul", {{1, 0}, {1, 0}}, attrs,
                               "/job:localhost/replica:0/task:0/device:CPU:0",
                               &remote_enqueue_request);

  TF_ASSERT_OK(eager_service_impl.Enqueue(&remote_enqueue_request,
                                          &remote_enqueue_response));

  const tensorflow::Tensor* t = nullptr;
  tensorflow::TensorHandle* tensor_handle;
  TF_ASSERT_OK(eager_service_impl.GetTensorHandle(
      context_id, RemoteTensorHandleInternal(2, 0), &tensor_handle));
  TF_ASSERT_OK(tensor_handle->Tensor(&t));

  Device* device = tensor_handle->device();
  EXPECT_EQ(device, nullptr);

  auto actual = t->flat<float>();
  EXPECT_EQ(4, actual.size());

  EXPECT_EQ(7, actual(0));
  EXPECT_EQ(10, actual(1));
  EXPECT_EQ(15, actual(2));
  EXPECT_EQ(22, actual(3));

  CloseContextRequest close_context_request;
  close_context_request.set_context_id(context_id);
  CloseContextResponse close_context_response;
  TF_ASSERT_OK(eager_service_impl.CloseContext(&close_context_request,
                                               &close_context_response));
}

// Test requests sent to the eager service on master.
TEST_F(EagerServiceImplTest, RequestsToMasterTest) {
  tensorflow::Rendezvous* rendezvous =
      new tensorflow::IntraProcessRendezvous(device_mgr_.get());
  // Create a master eager context.
  tensorflow::EagerContext* ctx = new tensorflow::EagerContext(
      SessionOptions(),
      tensorflow::ContextDevicePlacementPolicy::DEVICE_PLACEMENT_SILENT,
      tensorflow::ContextMirroringPolicy::MIRRORING_NONE, false,
      device_mgr_.get(), false, rendezvous, GetDefaultCustomKernelCreator(),
      nullptr);
  const uint64 context_id = random::New64();

  // Set RemoteMgr to ctx.
  auto remote_mgr =
      absl::make_unique<tensorflow::eager::RemoteMgr>(/*is_master=*/true, ctx);
  TF_ASSERT_OK(ctx->InitializeRemoteWorker(nullptr, nullptr, {}, context_id, 0,
                                           nullptr, std::move(remote_mgr)));

  TestEagerServiceImpl eager_service_impl(&worker_env_);

  EnqueueRequest remote_enqueue_request;
  remote_enqueue_request.set_context_id(context_id);
  EnqueueResponse remote_enqueue_response;

  auto* send_tensor = remote_enqueue_request.add_queue()->mutable_send_tensor();
  send_tensor->set_op_id(1);
  SetTensorProto(send_tensor->add_tensors());

  // Unable to handle the request since there is no eager context.
  Status status = eager_service_impl.Enqueue(&remote_enqueue_request,
                                             &remote_enqueue_response);
  EXPECT_EQ(error::INVALID_ARGUMENT, status.code());
  EXPECT_TRUE(absl::StrContains(
      status.error_message(),
      "Unable to find a context_id matching the specified one"));

  // The request can be handled after adding the master eager context to
  // service.
  TF_ASSERT_OK(eager_service_impl.CreateMasterContext(context_id, ctx));
  TF_ASSERT_OK(eager_service_impl.Enqueue(&remote_enqueue_request,
                                          &remote_enqueue_response));
  ctx->Unref();
}

TEST_F(EagerServiceImplTest, KeepAliveTest) {
  TestEagerServiceImpl eager_service_impl(&worker_env_);

  uint64 context_id = random::New64();
  CreateContextRequest request;
  request.mutable_server_def()->set_job_name("localhost");
  request.mutable_server_def()->set_task_index(0);
  request.set_context_id(context_id);
  request.set_keep_alive_secs(3);
  CreateContextResponse response;

  TF_ASSERT_OK(eager_service_impl.CreateContext(&request, &response));

  worker_env_.env->SleepForMicroseconds(5 *
                                        tensorflow::EnvTime::kSecondsToMicros);

  KeepAliveRequest keep_alive_request;
  KeepAliveResponse keep_alive_response;

  keep_alive_request.set_context_id(context_id);

  Status status =
      eager_service_impl.KeepAlive(&keep_alive_request, &keep_alive_response);

  EXPECT_EQ(status.code(), error::INVALID_ARGUMENT);
  EXPECT_PRED_FORMAT2(::testing::IsSubstring, "Unable to find a context_id",
                      status.error_message());

  uint64 new_context_id = random::New64();
  // Create a new context.
  request.set_context_id(new_context_id);
  TF_ASSERT_OK(eager_service_impl.CreateContext(&request, &response));

  // The context should not be GC'd.
  worker_env_.env->SleepForMicroseconds(1 *
                                        tensorflow::EnvTime::kSecondsToMicros);

  keep_alive_request.set_context_id(new_context_id);

  TF_ASSERT_OK(
      eager_service_impl.KeepAlive(&keep_alive_request, &keep_alive_response));
}

}  // namespace
}  // namespace eager
}  // namespace tensorflow
