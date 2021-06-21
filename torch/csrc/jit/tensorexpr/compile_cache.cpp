#include <pybind11/functional.h>
#include <pybind11/operators.h>
#include <pybind11/stl.h>
#include <torch/csrc/autograd/custom_function.h>
#include <torch/csrc/jit/python/pybind_utils.h>
#include <torch/csrc/jit/tensorexpr/codegen.h>
#include <torch/csrc/jit/tensorexpr/compile_cache.h>
#include <torch/csrc/jit/tensorexpr/ir_printer.h>
#include <torch/csrc/jit/tensorexpr/ir_simplifier.h>
#include <torch/csrc/jit/tensorexpr/kernel.h>
#include <torch/csrc/jit/tensorexpr/llvm_codegen.h>
#include <torch/csrc/jit/tensorexpr/loopnest.h>
#include <torch/csrc/jit/tensorexpr/reduction.h>
#include <array>
#include <cassert>
#include <map>
#include <mutex>
#include <vector>

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define AT __FILE__ ":" TOSTRING(__LINE__)
#define ASSERT(test) \
  if (!(test))       \
  throw std::runtime_error("assert failed " AT)

namespace torch {
namespace jit {
namespace {
using namespace torch::jit::tensorexpr;

class CompileCache;

class CCNode : public torch::autograd::Node {
  typedef torch::autograd::variable_list variable_list;

 public:
  variable_list apply(variable_list&& inputs) override {
    return {at::empty_like(inputs[0]).fill_(-99.0)};
  }
  // void release_variables() override {}

  static void setup(
      at::Tensor& output,
      const std::vector<at::Tensor>& input_vars) {
    std::shared_ptr<CCNode> node(new CCNode(), torch::autograd::deleteNode);

    auto next_edges = torch::autograd::collect_next_edges(
        std::vector<at::Tensor>{input_vars[1]});
    // node->set_ctx_grad_fn(node);
    node->set_next_edges(std::move(next_edges));

    node->clear_input_metadata();
    auto output_nr = node->add_input_metadata(output);
    torch::autograd::impl::set_gradient_edge(output, {node, output_nr});

    //  for(int i=0; i<input_vars.size(); ++i) {
    //    input_vars[i] = input_vars[i].detach();
    //  }
    //  // TODO: check is: modified

    //  torch::autograd::impl::set_gradient_edge(output, {node, output_nr});

    // node->input_info_.reserve(input_vars.size());
    // for (auto& var : input_vars) {
    //   node->input_info_.emplace_back(var);
    // }

    // auto result = fn();
    // using forward_return_t = forward_t<X, Args...>;
    // forward_return_t outputs;
    // {
    //   AutoGradMode grad_mode(false);
    //   outputs = T::forward(&node->ctx_, std::forward<Args>(args)...);
    // }

    // auto wrapped_outputs = _wrap_outputs(
    //    input_vars,
    //    {}, {},
    //    //node->ctx_.get_non_differentiable(),
    //    //node->ctx_.get_and_bump_dirty(),
    //    to_optional(outputs),
    //    node);

    // node->output_info_.reserve(wrapped_outputs.size());
    // for (auto& output : wrapped_outputs) {
    //  if (is_executable && output.has_value()) {
    //    node->output_info_.emplace_back(output.value());
    //  } else if (is_executable) {
    //    node->output_info_.emplace_back();
    //  }
    //}

    // if (is_executable) {
    //  node->save_variables_to_ctx();
    //}

    // return output;
  }
};

py::object python_specialization_key() {
  static py::object* rtype = nullptr;
  if (rtype == nullptr) {
    py::object namedtuple =
        py::module_::import("collections").attr("namedtuple");
    rtype = new py::object();
    *rtype = namedtuple(
        "SpecializationKey",
        "alias_group,ndim,dtype,device,layout,requires_grad,out,shape,stride");
  }
  return *rtype;
}

template <int MAX_DIMS>
class SpecializationKey {
 protected:
  enum DimFlags {
    SIZE_MISSING = 1 << 0, // leading dimension implicitly added
    SIZE_ONE = 1 << 1, // == 1
    SIZE_OTHER = 1 << 2, // > 1

    STRIDE_ZERO = 1 << 3, // == 0 (broadcast)
    STRIDE_ONE = 1 << 4, // == 1 (packed)
    STRIDE_CONTIGUOUS = 1 << 5, // stride[i+1] * sizes[i+1]
    STRIDE_TRANSPOSED_CONTIGUOUS = 1 << 6, // stride[i-1] * sizes[i-1]
    STRIDE_AS_ARG = 1 << 7,
  };
  static constexpr int MASK = (1 << 5) - 1;

  static inline uint16_t pack_flags(const at::Tensor& v, bool is_out) {
    // pack all the tensor properties into a uint16 for fast hash/compare
    constexpr uint16_t S0 = 1;
    constexpr uint16_t S1 = S0 * 2;
    constexpr uint16_t S2 = S1 * 2;
    constexpr uint16_t S3 = S2 * static_cast<int>(at::ScalarType::NumOptions);
    constexpr uint16_t S4 = S3 * static_cast<int>(at::Layout::NumOptions);
    constexpr uint16_t S5 =
        S4 * static_cast<int>(at::DeviceType::COMPILE_TIME_MAX_DEVICE_TYPES);
    static_assert(S3 < S4 && S4 < S5); // overflow check

    at::ScalarType dtype = v.dtype().toScalarType();
    at::DeviceType device = v.device().type();
    at::Layout layout = v.layout();
    bool requires_grad = v.requires_grad() && at::GradMode::is_enabled();

    return S0 * static_cast<uint16_t>(is_out) +
        S1 * static_cast<uint16_t>(requires_grad) +
        S2 * static_cast<uint16_t>(dtype) + S3 * static_cast<uint16_t>(layout) +
        S4 * static_cast<uint16_t>(device);
  }

  template <typename T>
  inline void init_dimflags(
      const T& sizes,
      const T& strides,
      int64_t ndims,
      void**& call_args_out) {
    // pack all the properties for each dimension into a uint8
    int out_idx = 0;
    for (int dim = 0; dim < ndims; ++dim) {
      uint8_t flag = (sizes[dim] == 1 ? SIZE_ONE : SIZE_OTHER);
      if (strides[dim] == 0) {
        flag |= STRIDE_ZERO;
      } else if (strides[dim] == 1) {
        flag |= STRIDE_ONE;
      } else if (
          dim + 1 < sizes.size() &&
          strides[dim] == strides[dim + 1] * sizes[dim + 1]) {
        flag |= STRIDE_CONTIGUOUS;
      } else if (
          dim > 0 && strides[dim] == strides[dim - 1] * sizes[dim - 1] &&
          (dimflags_[out_idx - 1] & STRIDE_CONTIGUOUS) == 0) {
        flag |= STRIDE_TRANSPOSED_CONTIGUOUS;
      } else {
        flag |= STRIDE_AS_ARG;
        *call_args_out++ = const_cast<int64_t*>(&strides[dim]);
      }
      dimflags_[out_idx++] = flag;
    }
    while (out_idx < MAX_DIMS) {
      dimflags_[out_idx++] = SIZE_MISSING | STRIDE_ZERO;
    }
  }

 public:
  SpecializationKey() {}

  SpecializationKey(
      const at::Tensor& v,
      int8_t alias_group,
      bool is_out,
      void**& call_args_out)
      : flags_(pack_flags(v, is_out)), alias_group_(alias_group) {
    init_dimflags(v.sizes(), v.strides(), v.ndimension(), call_args_out);
  }

  int cmp(const SpecializationKey<MAX_DIMS>& other) const {
    return memcmp(
        &flags_,
        &other.flags_,
        sizeof(flags_) + sizeof(alias_group_) + sizeof(dimflags_));
  }

  std::vector<std::string> shape() const {
    std::vector<std::string> result;
    for (int i = 0; i < MAX_DIMS; ++i) {
      if ((dimflags_[i] & SIZE_MISSING) > 0)
        break;

      if ((dimflags_[i] & SIZE_ONE) > 0)
        result.push_back("one");
      else
        result.push_back("other");
    }
    return result;
  }
  std::vector<std::string> stride() const {
    std::vector<std::string> result;
    for (int i = 0; i < MAX_DIMS; ++i) {
      if ((dimflags_[i] & SIZE_MISSING) > 0)
        break;

      if ((dimflags_[i] & STRIDE_ZERO) > 0)
        result.push_back("zero");
      else if ((dimflags_[i] & STRIDE_ONE) > 0)
        result.push_back("one");
      else if ((dimflags_[i] & STRIDE_CONTIGUOUS) > 0)
        result.push_back("contiguous");
      else if ((dimflags_[i] & STRIDE_TRANSPOSED_CONTIGUOUS) > 0)
        result.push_back("transposed_contiguous");
      else if ((dimflags_[i] & STRIDE_AS_ARG) > 0)
        result.push_back("as_arg");
      else
        throw std::runtime_error("??");
    }
    return result;
  }

  py::object to_python(const at::Tensor& example) const {
    py::object ex = py::cast(example);
    return python_specialization_key()(
        static_cast<int>(alias_group_),
        ex.attr("ndim"),
        ex.attr("dtype"),
        ex.attr("device"),
        ex.attr("layout"),
        ex.attr("requires_grad"),
        py::bool_(flags_ % 2),
        shape(),
        stride());
  }

 private:
  uint16_t flags_; // dtype, layout, device, and requires_grad
  int8_t alias_group_; // 0 = no aliasing
                       // >0 = same data, strides, and shapes within group
                       // <0 = overlapping storage madness
  uint8_t dimflags_[MAX_DIMS];
} __attribute__((packed));

class CompileResultBase : public KernelScopedObject {
 public:
  virtual ~CompileResultBase() = default;
  virtual void set_code(const py::object& cg) = 0;
  virtual void set_shape_from(
      const std::vector<std::pair<int, int>>& indices) = 0;
  virtual void add_allocated_output(
      int options_from,
      const std::vector<int>& storage_order) = 0;
  virtual void add_shape_check(
      const std::tuple<int, int, int, int>& indices) = 0;
  virtual void set_num_args(
      int buffer_args,
      int stride_args,
      int shape_args) = 0;
  virtual void set_backwards(int index, py::object backward_compiler) = 0;
};

struct CompileResultProxy {
  CompileResultBase* res;
  explicit CompileResultProxy(CompileResultBase* r) : res(r) {}
};

struct CmpLess {
  template <typename T>
  size_t operator()(const T& left, const T& right) const {
    for (int i = 0; i < left.size(); ++i) {
      auto c = left[i].cmp(right[i]);
      if (c < 0)
        return true;
      if (c > 0)
        return false;
    }
    return false;
  }
};

template <int NARGS, int MAX_DIMS>
class CompileCache3 {
 public:
  typedef SpecializationKey<MAX_DIMS> ArgKey;
  typedef std::array<ArgKey, NARGS> Key;
  typedef std::array<at::Tensor, NARGS> Args;
  typedef std::array<int8_t, NARGS> AliasGroups;

  class CompileResultImpl : public CompileResultBase {
   public:
    void set_code(const py::object& cg) override {
      objects_.push_back(cg);
      cg_ = cg.cast<CodeGen*>();
    }
    void set_shape_from(
        const std::vector<std::pair<int, int>>& indices) override {
      assert(indices.shape() <= MAX_DIMS);
      shape_from_ = indices;
    }

    void add_allocated_output(
        int options_from,
        const std::vector<int>& storage_order) override {
      if (allocated_outputs_.size() > 0) {
        throw std::runtime_error("TODO: support more than one output");
      }
      allocated_outputs_.push_back(std::make_pair(options_from, storage_order));
    }

    void add_shape_check(
        const std::tuple<int, int, int, int>& indices) override {
      shape_checks_.push_back(indices);
    }

    void set_num_args(int buffer_args, int stride_args, int shape_args)
        override {
      shape_args_offset_ = buffer_args + stride_args;
      num_args_ = buffer_args + stride_args + shape_args;
    }

    void set_backwards(int index, py::object backward_compiler) {
      objects_.push_back(backward_compiler);
      backwards_functions_.emplace_back(
          std::make_pair(index, (CompileCache*)nullptr));
    }

    at::Tensor call(at::Tensor* args, void** __restrict__ call_args) {
      for (const auto& ck : shape_checks_) {
        if (args[std::get<0>(ck)].size(std::get<1>(ck)) !=
            args[std::get<2>(ck)].size(std::get<3>(ck))) {
          // TODO(jansel): make this error message match eager
          throw std::runtime_error(
              "The size of tensor A must match the size of tensor B at non-singleton dimension X");
        }
      }

      int64_t shapes[MAX_DIMS];
      int ndims = std::min<int>(shape_from_.size(), MAX_DIMS);
      for (int i = 0; i < ndims; ++i) {
        shapes[i] = args[shape_from_[i].first].size(shape_from_[i].second);
        call_args[shape_args_offset_ + i] = &shapes[i];
      }

      for (int i = 0; i < NARGS; ++i) {
        call_args[i] = args[i].data_ptr();
      }
      at::Tensor output;
      if (allocated_outputs_.size() > 0) {
        int options_from = allocated_outputs_[0].first;
        auto& output_order = allocated_outputs_[0].second;
        int64_t strides[MAX_DIMS] = {0};
        int64_t next_stride = 1;
        for (int i : output_order) {
          strides[i] = next_stride;
          next_stride *= shapes[i];
        }
        output = at::empty_strided(
            c10::IntArrayRef(shapes, shapes + ndims),
            c10::IntArrayRef(strides, strides + ndims),
            args[options_from].options());
        call_args[NARGS] = output.data_ptr();
      } else {
        output = args[NARGS - 1];
      }

      cg_->call_raw(call_args, num_args_);

      if (backwards_functions_.size() > 0) {
        std::shared_ptr<CCNode> node(new CCNode(), torch::autograd::deleteNode);

        // node outputs
        torch::autograd::edge_list next_edges;
        for (auto& item : backwards_functions_) {
          int index = item.first;
          next_edges.emplace_back(
              torch::autograd::impl::gradient_edge(args[index]));
        }
        node->set_next_edges(std::move(next_edges));

        // node inputs
        torch::autograd::create_gradient_edge(output, node);
      }

      return output;
    }

   private:
    CodeGen* cg_ = nullptr;
    int shape_args_offset_ = 0;
    int num_args_ = 0;
    std::vector<std::pair<int, int>> shape_from_;
    std::vector<std::tuple<int, int, int, int>> shape_checks_;
    std::vector<std::pair<int, std::vector<int>>> allocated_outputs_;
    std::vector<std::pair<int, CompileCache*>> backwards_functions_;
    std::vector<py::object> objects_; // for ref counting
  };
  typedef std::map<Key, CompileResultImpl*, CmpLess> Map;

  CompileResultImpl* cached_compile(const Key& key, at::Tensor* args) {
    std::lock_guard<std::mutex> guard(mutex_);
    auto item = cache_.find(key);
    if (item != cache_.end()) {
      return item->second;
    } else {
      KernelScope scope(&arena_);
      auto cr = new CompileResultImpl();
      std::vector<py::object> spec;
      for (int i = 0; i < key.size(); ++i) {
        spec.push_back(key[i].to_python(args[i]));
      }
      compile_fn_(spec, CompileResultProxy(cr));
      cache_.emplace(std::make_pair(key, cr));
      return cr;
    }
  }

  int8_t aliasing_check(const at::Tensor& a, const at::Tensor& b) {
    if (a.is_alias_of(b)) {
      if (a.is_set_to(b)) {
        return 1;
      } else {
        // TODO: check for non-overlapping and return 0
        //       likely we could lift some logic from tensoriterator
        return -1;
      }
    } else {
      return 0;
    }
  }

  AliasGroups compute_alias_groups(at::Tensor* args) {
    AliasGroups alias_groups;
    int8_t current_id = 0;
    for (int i = 0; i < NARGS; ++i) {
      alias_groups[i] = 0;
    }
    for (int i = 0; i < NARGS; ++i) {
      if (alias_groups[i] == 0) {
        for (int j = i + 1; j < NARGS; ++j) {
          int8_t alias_type = aliasing_check(args[i], args[j]);
          if (alias_type != 0) {
            if (alias_groups[i] == 0)
              ++current_id;
            alias_groups[i] = current_id;
            alias_groups[j] = current_id * alias_type;
          }
        }
      }
    }
    return alias_groups;
  }

  Key compute_cache_key(at::Tensor* args, bool has_out, void** call_args_out) {
    AliasGroups alias_groups = compute_alias_groups(args);
    Key key;
    int i = 0;
    for (; i < NARGS - 1; ++i) {
      key[i] = ArgKey(args[i], alias_groups[i], false, call_args_out);
    }
    if (NARGS != 0) {
      key[i] = ArgKey(args[i], alias_groups[i], has_out, call_args_out);
    }
    return key;
  }

  CompileCache3(const py::object& compile_fn) : compile_fn_(compile_fn) {}

  at::Tensor call(at::Tensor* args, bool has_out) {
    void* call_args[NARGS + (!has_out) + NARGS * MAX_DIMS + MAX_DIMS];
    auto key = compute_cache_key(args, has_out, call_args + NARGS + (!has_out));
    return cached_compile(key, args)->call(args, call_args);
  }

 public:
  std::mutex mutex_;
  Map cache_;
  py::object compile_fn_;
  KernelArena arena_;
};

template <int NARGS>
class CompileCache2 {
 public:
  CompileCache2(const py::object& compile_fn)
      : cache2(compile_fn), cache4(compile_fn), cache8(compile_fn) {}

  at::Tensor call(at::Tensor* args, bool has_out) {
    // fan out and and specialize on number of dimension buckets
    int64_t ndims = 0;
    for (int i : c10::irange(NARGS)) {
      ndims = std::max(args[i].dim(), ndims);
    }
    if (ndims <= 2)
      return cache2.call(args, has_out);
    if (ndims <= 4)
      return cache4.call(args, has_out);
    if (ndims <= 8)
      return cache8.call(args, has_out);
    throw std::runtime_error("TODO: handle more dims");
  }

 private:
  CompileCache3<NARGS, 2> cache2;
  CompileCache3<NARGS, 4> cache4;
  CompileCache3<NARGS, 8> cache8;
};

class CompileCache {
 public:
  virtual ~CompileCache() = default;
  virtual at::Tensor call(py::args args, py::kwargs kwargs) = 0;
};

template <int NARGS>
class CompileCacheImpl : public CompileCache {
  constexpr static int MAX_ARGS = 5;

  struct Cleanup {
    at::Tensor* tensors;
    int count;

    inline ~Cleanup() {
      for (int i : c10::irange(count)) {
        tensors[i].~Tensor();
      }
    }
  };

 public:
  CompileCacheImpl(const py::object& compile_fn)
      : cache(compile_fn), cache_out(compile_fn) {}

  at::Tensor call(py::args args, py::kwargs kwargs) {
    // fan out an specialize on arg counts
    int num_args = py::len(args);
    int num_kwargs = py::len(kwargs);
    if (C10_UNLIKELY(num_kwargs > 1 || num_args != NARGS)) {
      throw std::runtime_error("wrong number of args");
    }

    char tensor_args_buffer[sizeof(at::Tensor) * (NARGS + 1)];
    at::Tensor* tensor_args = reinterpret_cast<at::Tensor*>(tensor_args_buffer);

    for (int i = 0; i < NARGS; ++i) {
      new (tensor_args + i) at::Tensor(std::move(args[i].cast<at::Tensor>()));
    }

    if (num_kwargs == 1) {
      new (tensor_args + NARGS)
          at::Tensor(std::move(kwargs["out"].cast<at::Tensor>()));
      Cleanup call_dtors = {tensor_args, NARGS + 1};
      return cache_out.call(tensor_args, true);
    } else {
      Cleanup call_dtors = {tensor_args, NARGS};
      return cache.call(tensor_args, false);
    }
  }

 private:
  CompileCache2<NARGS> cache;
  CompileCache2<NARGS + 1> cache_out; // out variant
};

CompileCache* create_compile_cache(const py::object& compile_fn, int num_args) {
  switch (num_args) {
    case 1:
      return new CompileCacheImpl<1>(compile_fn);
    case 2:
      return new CompileCacheImpl<2>(compile_fn);
    case 3:
      return new CompileCacheImpl<3>(compile_fn);
    case 4:
      return new CompileCacheImpl<4>(compile_fn);
    case 5:
      return new CompileCacheImpl<5>(compile_fn);
    case 6:
      return new CompileCacheImpl<6>(compile_fn);
    default:
      throw std::runtime_error("TODO: support other arg counts");
  }
}

} // namespace

void initTensorExprAuthoringBindings(PyObject* te_obj) {
  py::handle te(te_obj);

  py::class_<CompileCache>(te, "CompileCache")
      .def(py::init(&create_compile_cache))
      .def("__call__", &CompileCache::call);

  py::class_<CompileResultProxy>(te, "CompileResult")
      .def(
          "set_code",
          [](CompileResultProxy& self, const py::object& cg) {
            self.res->set_code(cg);
          })
      .def(
          "add_shape_check",
          [](CompileResultProxy& self,
             const std::tuple<int, int, int, int>& indices) {
            self.res->add_shape_check(indices);
          })
      .def(
          "set_shape_from",
          [](CompileResultProxy& self,
             const std::vector<std::pair<int, int>>& indices) {
            self.res->set_shape_from(indices);
          })
      .def(
          "set_num_args",
          [](CompileResultProxy& self,
             int buffer_args,
             int stride_args,
             int shape_args) {
            self.res->set_num_args(buffer_args, stride_args, shape_args);
          })
      .def(
          "add_allocated_output",
          [](CompileResultProxy& self,
             int options_from,
             const std::vector<int>& storage_order) {
            self.res->add_allocated_output(options_from, storage_order);
          })
      .def(
          "set_backwards",
          [](CompileResultProxy& self,
             int index,
             py::object backward_compiler) {
            self.res->set_backwards(index, backward_compiler);
          });
}
} // namespace jit
} // namespace torch
