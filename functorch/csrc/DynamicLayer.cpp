// Copyright (c) Facebook, Inc. and its affiliates.
// All rights reserved.
//
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree.

#include <functorch/csrc/DynamicLayer.h>
#include <functorch/csrc/TensorWrapper.h>
#include <functorch/csrc/BatchedTensorImpl.h>

#include <torch/library.h>
#include <c10/core/impl/LocalDispatchKeySet.h>
#include <ATen/core/dispatch/Dispatcher.h>
#include <torch/csrc/autograd/variable.h>
#include <c10/util/irange.h>
#include <ATen/FuncTorchTLS.h>

namespace at {
namespace functorch {

constexpr DispatchKeySet all_dynlayer_keyset = DispatchKeySet({
  kDynamicLayerFrontModeKey,
  kDynamicLayerBackModeKey,
  kGradWrapperKey,
  // DispatchKey::Batched,
  kBatchedKey,
  DispatchKey::ADInplaceOrView
}) | autograd_dispatch_keyset;

void setDynamicLayerFrontBackKeysIncluded(bool included) {
  c10::impl::tls_set_dispatch_key_included(kDynamicLayerFrontModeKey, included);
  c10::impl::tls_set_dispatch_key_included(kDynamicLayerBackModeKey, included);
}

DynamicLayer::DynamicLayer(
    DispatchKey key,
    int64_t layerId,
    optional<int64_t> batchSize,
    optional<RandomnessType> randomness,
    optional<bool> prev_grad_mode,
    optional<bool> prev_fwd_grad_mode)
  :
    key_(key),
    layerId_(layerId),
    batchSize_(batchSize),
    randomness_(randomness),
    prevGradMode_(prev_grad_mode),
    prevFwdGradMode_(prev_fwd_grad_mode),
    prevLocalDispatchKeySet_(c10::impl::tls_local_dispatch_key_set())
{
  if (key_ == DispatchKey::Autograd) {
    TORCH_INTERNAL_ASSERT(prev_grad_mode.has_value() || prev_fwd_grad_mode.has_value());
  }
}

DispatchKey DynamicLayer::key() const {
  return key_;
}

int64_t DynamicLayer::layerId() const {
  return layerId_;
}

int64_t DynamicLayer::batchSize() const {
  TORCH_INTERNAL_ASSERT(batchSize_);
  return *batchSize_;
}

RandomnessType DynamicLayer::randomness() const {
  TORCH_INTERNAL_ASSERT(randomness_);
  return *randomness_;
}

optional<bool> DynamicLayer::prevGradMode() const {
  return prevGradMode_;
}

optional<bool> DynamicLayer::prevFwdGradMode() const {
  return prevFwdGradMode_;
}

c10::impl::LocalDispatchKeySet DynamicLayer::prevLocalDispatchKeySet() const {
  return prevLocalDispatchKeySet_;
}

using DynmetaData = std::unordered_map<int64_t, std::shared_ptr<bool>>;
DynmetaData kDynMetaDataSingleton;

static DynmetaData& getGlobalDynmetaData() {
  return kDynMetaDataSingleton;
}

class FuncTorchTLS : public FuncTorchTLSBase {
 public:
  FuncTorchTLS() {}

  std::unique_ptr<FuncTorchTLSBase> deepcopy() const override {
    auto result = std::make_unique<FuncTorchTLS>();
    result->dynamicLayerStack = dynamicLayerStack;
    return result;
  }

  int64_t checkSupportsAutogradFunction() const override {
    TORCH_CHECK(dynamicLayerStack.size() <= 1, // we're inside a transform if the stack has more than the inital layer
        "functorch functions (vmap, grad, vjp, etc.) currently do not support the use of autograd.Function. ",
        "Please rewrite your function to not use autograd.Function while we work on fixing this");
    return 0;
  }

  void checkSupportsInplaceRequiresGrad() const override {
    // Does nothing
  }
  void checkSupportsRetainGrad() const override {
    // Does nothing
  }

  std::vector<DynamicLayer> dynamicLayerStack;
};

static FuncTorchTLS* getRawFunctorchTLS() {
  auto& state = functorchTLSAccessor();
  if (state == nullptr) {
    state = std::make_unique<FuncTorchTLS>();
  }
  // Raw pointer usage OK, `state` keeps the pointer alive
  FuncTorchTLSBase* raw_state = state.get();
  FuncTorchTLS* result = static_cast<FuncTorchTLS*>(raw_state);
  return result;
}

static std::vector<DynamicLayer>& dynamicLayerStackAccessor() {
  return getRawFunctorchTLS()->dynamicLayerStack;
}

std::shared_ptr<bool> getLifeHandleForLevel(int64_t level) {
  auto it = getGlobalDynmetaData().find(level);
  TORCH_INTERNAL_ASSERT(it != kDynMetaDataSingleton.end(), "level should be alive");
  return it->second;
}

optional<DynamicLayer> maybeCurrentDynamicLayer() {
  auto& dynamicLayerStack = dynamicLayerStackAccessor();
  if (dynamicLayerStack.size() == 0) {
    return {};
  }
  return dynamicLayerStack.back();
}

const std::vector<DynamicLayer>& getDynamicLayerStack() {
  return dynamicLayerStackAccessor();
}

void setDynamicLayerStack(const std::vector<DynamicLayer>& stack) {
  dynamicLayerStackAccessor() = stack;
}

bool areTransformsActive() {
  const auto& data = getGlobalDynmetaData();
  return !data.empty();
}

static DynamicLayer popDynamicLayer() {
  auto& dynamicLayerStack = dynamicLayerStackAccessor();
  TORCH_INTERNAL_ASSERT(dynamicLayerStack.size() > 0);
  auto result = dynamicLayerStack.back();
  TORCH_INTERNAL_ASSERT(result.key() != DispatchKey::Undefined);
  dynamicLayerStack.pop_back();

  if (dynamicLayerStack.size() == 0) {
#ifdef HAS_TORCH_SHOW_DISPATCH_TRACE
    if (c10::show_dispatch_trace_enabled()) {
      std::cout << "DynamicLayer off" << std::endl;
    }
#endif
    setDynamicLayerFrontBackKeysIncluded(false);
  }

  return result;
}

static int64_t pushDynamicLayer(DynamicLayer&& dynamic_layer) {
  auto& dynamicLayerStack = dynamicLayerStackAccessor();
  int64_t layerId = 1 + dynamicLayerStack.size();
  TORCH_INTERNAL_ASSERT(layerId == dynamic_layer.layerId());
  dynamicLayerStack.emplace_back(dynamic_layer);

  if (layerId == 1) {
    setDynamicLayerFrontBackKeysIncluded(true);
  }

  return layerId;
}

int64_t initAndPushDynamicLayer(
    DispatchKey key,
    optional<int64_t> batch_size,
    optional<RandomnessType> randomness,
    optional<bool> prev_grad_mode,
    optional<bool> prev_fwd_grad_mode) {
  TORCH_INTERNAL_ASSERT(key == DispatchKey::Autograd || key == kBatchedKey);
  const auto& dynamicLayerStack = dynamicLayerStackAccessor();
  const auto layerId = 1 + dynamicLayerStack.size();
  DynamicLayer new_layer(key, layerId, batch_size, randomness, prev_grad_mode, prev_fwd_grad_mode);
  pushDynamicLayer(std::move(new_layer));

  auto& data = getGlobalDynmetaData();

  TORCH_INTERNAL_ASSERT(data.find(layerId) == data.end());
  if (key == DispatchKey::Autograd) {
    TORCH_INTERNAL_ASSERT(prev_grad_mode.has_value() || prev_fwd_grad_mode.has_value());
  }
  data[layerId] = std::make_shared<bool>(true);
  return layerId;
}

DynamicLayer popDynamicLayerAndDeleteMetadata() {
  auto result = popDynamicLayer();
  auto level = result.layerId();

  // TODO: is this lock safe? No one else should be writing to the same bucket
  // if (c10::show_dispatch_trace_enabled()) {
  //   std::cout << "deleting metadata" << std::endl;
  // }
  auto& data = getGlobalDynmetaData();
  auto it = data.find(level);
  if (it == data.end()) {
    return result;
  }
  // if (c10::show_dispatch_trace_enabled()) {
  //   std::cout << "deleted metadata for level " << level << std::endl;
  // }
  // invalidate the thing
  *(it->second) = false;
  data.erase(level);
  return result;
}

static Tensor materializeGradWrappers(const Tensor& tensor, const std::vector<DynamicLayer>& dynlayerStack) {
  if (!tensor.defined()) {
    return tensor;
  }
  if (dynlayerStack.back().key() != DispatchKey::Autograd) {
    return tensor;
  }
  auto cur_level = dynlayerStack.back().layerId();
  auto* wrapper = maybeGetTensorWrapper(tensor);
  if (!wrapper) {
    return makeTensorWrapper(tensor, cur_level);
  }
  TORCH_INTERNAL_ASSERT(wrapper->level().value() <= cur_level, "escaped?");
  if (wrapper->level().value() == cur_level) {
    TORCH_INTERNAL_ASSERT(tensor.defined());
    return tensor;
  }
  return makeTensorWrapper(tensor, cur_level);
}

static Tensor unwrapIfDead(const Tensor& tensor) {
  auto* wrapped = maybeGetTensorWrapper(tensor);
  if (!wrapped) {
    return tensor;
  }
  if (wrapped->is_alive()) {
    return tensor;
  }
  return wrapped->value();
}

void foreachTensorInplace(std::vector<IValue>& args, int64_t begin, int64_t end,
    std::function<Tensor(const Tensor&)> func) {
  TORCH_INTERNAL_ASSERT(begin >= 0);
  TORCH_INTERNAL_ASSERT(end >= 0);
  TORCH_INTERNAL_ASSERT(begin <= end);
  for (int64_t idx = begin; idx < end; idx++) {
    auto ivalue = args[idx];
    // Tensor?[] translates to a c10::List<IValue> so we need to peek inside List
    if (ivalue.isList()) {
      bool modified = false;
      // TODO: might be more efficient if we scan first then not copy? Depends.
      auto list = ivalue.toList().copy();
      for (const auto list_idx : c10::irange(0, list.size())) {
        const auto& elt = list.get(list_idx);
        if (elt.isTensor()) {
          list.set(list_idx, func(elt.toTensor()));
          modified = true;
        }
      }
      if (modified) {
        args[idx] = list;
      }
      continue;
    }
    if (ivalue.isTensorList()) {
      auto list = ivalue.toTensorList();
      for (const auto list_idx : c10::irange(0, list.size())) {
        list[list_idx] = func(list[list_idx]);
      }
      args[idx] = list;
    }
    TORCH_INTERNAL_ASSERT(!ivalue.isGenericDict(), "No operators can accept GenericDict");
    if (!ivalue.isTensor()) {
      continue;
    }
    Tensor value = ivalue.toTensor();
    Tensor replacement = func(value);
    args[idx] = std::move(replacement);
    // sanity checks
    if (ivalue.toTensor().defined()) {
      TORCH_INTERNAL_ASSERT(args[idx].toTensor().defined());
    }
  }
}

std::ostream& operator<< (std::ostream& os, const DynamicLayer& layer) {
  os << layer.layerId() << ":" << layer.key();
  return os;
}
std::ostream& operator<< (std::ostream& os, const std::vector<DynamicLayer>& dls) {
  os << "DynamicLayerStack[ ";
  for (const auto& layer : dls) {
    os << layer << " ";
  }
  os << "]";
  return os;
}

static void sanityCheckStack(const c10::OperatorHandle& op, torch::jit::Stack* stack) {
  auto num_args = op.schema().arguments().size();
  foreachTensorInplace(*stack, stack->size() - num_args, stack->size(),
      [](const Tensor& tensor) {

        auto* wrapper = maybeGetTensorWrapper(tensor);
        TORCH_INTERNAL_ASSERT(wrapper == nullptr);
        auto* batched = maybeGetBatchedImpl(tensor);
        TORCH_INTERNAL_ASSERT(batched == nullptr);
        return tensor;
      });
}

bool isInplaceOp(const FunctionSchema& schema) {
  if (!schema.is_mutable() || schema.returns().size() != 1) {
    return false;
  }
  // Check that the first argument is being written to
  const auto& first_arg_alias_info = schema.arguments().begin()->alias_info();
  if (!first_arg_alias_info || !first_arg_alias_info->isWrite()) {
    return false;
  }
  // Check that none of the other args are being aliased
  for (auto it = schema.arguments().begin() + 1; it != schema.arguments().end(); ++it) {
    const auto& alias_info = it->alias_info();
    if (alias_info) {
      return false;
    }
  }
  // Check that the first tensor is being returned (i.e., output has a (a!))
  const auto& return_alias_info = schema.returns()[0].alias_info();
  return return_alias_info && return_alias_info->isWrite();
}

static void checkForInvalidMutationOnCaptures(
    const c10::OperatorHandle& op,
    torch::jit::Stack* stack,
    const std::vector<DynamicLayer>& dynamicLayerStack) {
  if (dynamicLayerStack.back().key() != DispatchKey::Autograd) {
    return;
  }
  if (!isInplaceOp(op.schema())) {
    return;
  }
  auto args = torch::jit::last(stack, op.schema().arguments().size());
  auto mutated_arg = unwrapIfDead(args[0].toTensor());
  auto cur_level = dynamicLayerStack.back().layerId();
  auto* wrapper = maybeGetTensorWrapper(mutated_arg);
  if (wrapper && wrapper->level().has_value() && wrapper->level().value() == cur_level) {
    return;
  }
  TORCH_CHECK(false,
      "During a grad (vjp, jvp, grad, etc) transform, the function provided ",
      "attempted to call in-place operation (", op.schema().operator_name(), ") ",
      "that would mutate a captured Tensor. This is not supported; please rewrite ",
      "the function being transformed to explicitly accept the mutated Tensor(s) ",
      "as inputs.");
}

static DispatchKeySet keysForEnteringDynamicLayer(DispatchKey key) {
  if (key == kBatchedKey) {
    // NB: Does not include kVmapModeKey. We may modulate the key when
    // constructing the DynamicLayer, but we don't control it when entering/exiting
    // the DynamicLayer.
    return DispatchKeySet({kBatchedKey});
  } else if (key == DispatchKey::Autograd) {
    return autograd_dispatch_keyset.add(DispatchKey::ADInplaceOrView);
  } else {
    TORCH_INTERNAL_ASSERT(false, "Unsupported key: ", key);
  }
}

static void dump_local_tls() {
  auto tls = c10::impl::tls_local_dispatch_key_set();
  std::cout << "[Local Include] " << tls.included_ << std::endl;
  std::cout << "[Local Exclude] " << tls.excluded_ << std::endl;
}

static void dump_tls(c10::impl::LocalDispatchKeySet tls) {
  std::cout << "[LocalDispatchKeySet]" << std::endl;
  std::cout << "[Local Include] " << tls.included_ << std::endl;
  std::cout << "[Local Exclude] " << tls.excluded_ << std::endl;
}

// The local dispatch keyset with all keys in all_dynlayer_keyset
// in the exclude set and not in the include set
static c10::impl::LocalDispatchKeySet zeroedOutDynamicLayerKeyset() {
  auto keyset = c10::impl::tls_local_dispatch_key_set();
  keyset.excluded_ = keyset.excluded_ | all_dynlayer_keyset;
  keyset.included_ = keyset.included_ - all_dynlayer_keyset;
  return keyset;
}

// Enable a set of keys by removing it from the exclude.
// Also, enable DynamicLayerBackMode so we can catch the subsystem on exit.
static c10::impl::LocalDispatchKeySet keysetToTurnOnSubsystem(DispatchKey key) {
  auto keyset = zeroedOutDynamicLayerKeyset();
  keyset.excluded_ = keyset.excluded_ - keysForEnteringDynamicLayer(key);
  keyset.excluded_ = keyset.excluded_.remove(kDynamicLayerBackModeKey);
  keyset.included_ = keyset.included_.add(kDynamicLayerBackModeKey);
  return keyset;
}

// Enable a set of keys such that on any dispatcher call we immediately
// get to the DynamicLayerFrontMode
static c10::impl::LocalDispatchKeySet keysetToReturnToDynamicLayerFront() {
  auto keyset = c10::impl::tls_local_dispatch_key_set();
  keyset.excluded_ = keyset.excluded_.remove(kDynamicLayerFrontModeKey);
  keyset.included_ = keyset.included_.add(kDynamicLayerFrontModeKey);
  return keyset;
}

void dynamicLayerFrontFallback(const c10::OperatorHandle& op, torch::jit::Stack* stack) {
  const auto& dynamicLayerStack = dynamicLayerStackAccessor();
#ifdef HAS_TORCH_SHOW_DISPATCH_TRACE
  if (c10::show_dispatch_trace_enabled()) {
    std::cout << dynamicLayerStack << std::endl;
  }
#endif
  TORCH_INTERNAL_ASSERT(dynamicLayerStack.size() > 0);

  // if is a grad transform, and the operation is in-place, and the mutated
  // argument is not currently wrapped in a TensorWrapper, then we need to
  // error out otherwise the result is silently incorrect
  checkForInvalidMutationOnCaptures(op, stack, dynamicLayerStack);

  // Unwrap dead GradWrappers, materialize live ones
  auto maybeTransformGradWrappers = [](const Tensor& tensor) {
    auto result = unwrapIfDead(tensor);
    return materializeGradWrappers(result, getDynamicLayerStack());
  };
  auto num_args = op.schema().arguments().size();
  foreachTensorInplace(*stack, stack->size() - num_args,
      stack->size(), maybeTransformGradWrappers);

  // dispatch key selection
  const auto& layer = dynamicLayerStack.back();
  auto selected_keyset = keysetToTurnOnSubsystem(layer.key());
  // hack. TODO: figure out how modes factor into this system...
  if (layer.key() == kBatchedKey) {
    selected_keyset.included_ = selected_keyset.included_.add(kVmapModeKey);
  }
  c10::impl::ForceDispatchKeyGuard guard(selected_keyset);

  // Re-dispatch
  op.callBoxed(stack);
}

struct WithoutTop {
  WithoutTop(): layer_(popDynamicLayer()) {
  }
  ~WithoutTop() {
    pushDynamicLayer(std::move(layer_));
  }

  DynamicLayer layer_;
};

struct SaveLocalDispatchKeySet {
 public:
  SaveLocalDispatchKeySet() :
    saved_keyset_(c10::impl::tls_local_dispatch_key_set()) {}
  ~SaveLocalDispatchKeySet() {
    c10::impl::_force_tls_local_dispatch_key_set(saved_keyset_);
  }

 private:
  c10::impl::LocalDispatchKeySet saved_keyset_;
};

void dynamicLayerBackFallback(const c10::OperatorHandle& op, torch::jit::Stack* stack) {
  auto cur_level = getDynamicLayerStack().back().layerId();
  auto cur_key = getDynamicLayerStack().back().key();
  auto tls_before_entering_functorch = getDynamicLayerStack().back().prevLocalDispatchKeySet();

  optional<bool> prev_grad_mode = getDynamicLayerStack().back().prevGradMode();
  optional<bool> prev_fwd_grad_mode = getDynamicLayerStack().back().prevFwdGradMode();
  if (cur_key == DispatchKey::Autograd) {
    TORCH_INTERNAL_ASSERT(prev_grad_mode.has_value() || prev_fwd_grad_mode.has_value());
  }

  auto unwrap = [&](const Tensor& tensor) {
    if (!tensor.defined()) {
      return tensor;
    }
    auto* maybe_tensor_wrapper = maybeGetTensorWrapper(tensor);
    if (!maybe_tensor_wrapper) {
      return tensor;
    }
    auto tensor_wrapper_level = maybe_tensor_wrapper->level().value();
    TORCH_INTERNAL_ASSERT(tensor_wrapper_level <= cur_level);
    if (tensor_wrapper_level == cur_level) {
      return maybe_tensor_wrapper->value();
    }
    return tensor;
  };
  auto wrap = [&](const Tensor& tensor) {
    if (!tensor.defined()) {
      return tensor;
    }
    // if (cur_level == 1) {
    //   return tensor;
    // }
    // if (c10::show_dispatch_trace_enabled()) {
    //   std::cout << "wrap " << cur_level << std::endl;
    // }
    return makeTensorWrapper(tensor, cur_level);
  };

  // TODO: we only need to do the following (marked with !) on in-place functions
  // that modify sizes or strides. There aren't many of them.
  // If autograd dispatch key:
  // 1. (!) Put a copy of all of the args onto the stack
  // 2. Unwrap all the args in the copy set
  // 3. Call the operator
  // 4. Wrap the output
  // 5. (!) refreshMetadata for all the args in the original set
  // 6. (!) Pop those args off.

  // Step 1 & 2
  if (cur_key == DispatchKey::Autograd) {
    auto args_size = op.schema().arguments().size();
    // Step 1
    auto front = stack->size() - args_size;
    for (const auto arg_idx : c10::irange(0, args_size)) {
      stack->push_back((*stack)[front + arg_idx]);
    }
    // Step 2
    foreachTensorInplace(*stack, stack->size() - args_size, stack->size(), unwrap);
  }

  // pop the top layer. Put it back on dtor.
  WithoutTop guard;

  optional<c10::AutoGradMode> grad_guard;
  if (cur_key == DispatchKey::Autograd && prev_grad_mode.has_value() &&
      *prev_grad_mode == false) {
    grad_guard.emplace(*prev_grad_mode);
  }

  optional<c10::AutoFwGradMode> fw_grad_guard;
  if (cur_key == DispatchKey::Autograd &&
      prev_fwd_grad_mode.has_value() && prev_fwd_grad_mode.value() == false) {
    fw_grad_guard.emplace(*prev_fwd_grad_mode);
  }

  // If there are no more layers in the DynamicLayerStack, then we want to
  // turn off functorch and do a dispatcher call. Otherwise, we're going to hop
  // to DynamicLayerFrontMode so that functorch can process the next layer.
  optional<c10::impl::ForceDispatchKeyGuard> tls_guard;
  if (getDynamicLayerStack().size() == 0) {
		tls_guard.emplace(tls_before_entering_functorch);
  } else {
    auto local_keyset = keysetToReturnToDynamicLayerFront();
    c10::impl::_force_tls_local_dispatch_key_set(local_keyset);
  }
#ifdef HAS_TORCH_SHOW_DISPATCH_TRACE
  if (c10::show_dispatch_trace_enabled() && tls_guard.has_value()) {
    std::cout << "[Exiting DynamicLayer]" << std::endl;
  }
#endif
	op.callBoxed(stack);
#ifdef HAS_TORCH_SHOW_DISPATCH_TRACE
  if (c10::show_dispatch_trace_enabled() && tls_guard.has_value()) {
    std::cout << "[Re-entering DynamicLayer]" << std::endl;
  }
#endif

  // Step 4, 5, 6
  if (cur_key == DispatchKey::Autograd) {
    // Step 4
    auto ret_size = op.schema().returns().size();
    foreachTensorInplace(*stack, stack->size() - ret_size, stack->size(), wrap);

    // Step 5
    auto args_size = op.schema().arguments().size();
    auto args_front = stack->size() - args_size - ret_size;
    for (const auto arg_idx : c10::irange(0, args_size)) {
      auto& ivalue = (*stack)[args_front + arg_idx];
      if (!ivalue.isTensor()) {
        continue;
      }
      auto maybe_tensor_wrapper = maybeGetTensorWrapper(ivalue.toTensor());
      if (!maybe_tensor_wrapper) {
        continue;
      }
      maybe_tensor_wrapper->refreshMetadata();
    }

    // Step 6
    stack->erase(stack->end() - (args_size + ret_size), stack->end() - ret_size);
  }
}

TORCH_LIBRARY_IMPL(_, FT_DYNAMIC_LAYER_FRONT_MODE_KEY, m) {
  m.fallback(torch::CppFunction::makeFromBoxedFunction<&dynamicLayerFrontFallback>());
}

TORCH_LIBRARY_IMPL(_, FT_DYNAMIC_LAYER_BACK_MODE_KEY, m) {
  m.fallback(torch::CppFunction::makeFromBoxedFunction<&dynamicLayerBackFallback>());
}

// TORCH_LIBRARY_IMPL(aten, DynamicLayerFront, m) {
//   m.impl("_unwrap_for_grad", native::_unwrap_for_grad);
//   m.impl("dump_tensor", native::dump_tensor);
//   m.impl("dlevel", native::dlevel);
// }

}
} // namespace at
