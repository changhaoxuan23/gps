// glaunch-module-manager - Manager interface for modules in glaunch
// Copyright (C) 2025 Haoxuan Chang<changhaoxuan23@mails.ucas.ac.cn>

// This is part of gps.
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.

// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#ifndef GPS_GLAUNCH_MODULE_MANAGER_HH_
#define GPS_GLAUNCH_MODULE_MANAGER_HH_

#include <configuration.hh>
#include <glaunch-hooks.hh>
#include <glaunch-protocols.hh>

#include <algorithm>
#include <any>
#include <array>
#include <concepts>
#include <functional>
#include <iterator>
#include <memory>
#include <span>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
namespace GPS {
namespace Internal {
template <std::integral T> struct basic_range {
private:
  struct range_iterator {
    T value;

    auto operator*() -> T { return this->value; }
    auto operator++() -> range_iterator & {
      ++this->value;
      return *this;
    }
    auto operator++(int) -> range_iterator {
      auto result = range_iterator{this->value++};
      return result;
    }
    auto operator!=(const range_iterator other) -> bool { return this->value != other.value; }
  };

public:
  using size_type = T;
  T start_;
  T end_;

  basic_range() = default;
  basic_range(T end);
  basic_range(T start, T end);

  [[nodiscard]] auto begin() const -> range_iterator { return range_iterator{this->start_}; };
  [[nodiscard]] auto end() const -> range_iterator { return range_iterator{this->end_}; };
  [[nodiscard]] auto size() const -> size_t { return this->end_ - this->start_; }
};
using range = basic_range<uint16_t>;
} // namespace Internal
// specification of callable and running ordering requirements of certain hook
template <HookID hook_id> struct ModuleHookSpecification {
  hook_callable_t<hook_id>    callable;
  // name of modules that this callable must run after
  std::span<std::string_view> after;
  // name of modules that this callable must run before
  std::span<std::string_view> before;
};

// information about hooks implemented by the module
struct ModuleHooks {
private:
  static constexpr const auto SpecificationSize = sizeof(ModuleHookSpecification<static_cast<HookID>(0)>);

  // constructor helper
  //  this method initializes empty (default constructed) ModuleHookSpecification at each slot
  template <typename T, T... ids> void initialize_helper(std::integer_sequence<T, ids...>) {
    (std::construct_at(
       reinterpret_cast<ModuleHookSpecification<static_cast<HookID>(ids)> *>(
         this->hooks.data() + ids * SpecificationSize
       )
     ),
     ...);
  }

  template <typename T, T... ids>
  void
  move_helper(std::array<uint8_t, SpecificationSize * HookCount> &other, std::integer_sequence<T, ids...>) {
    (std::uninitialized_move_n(
       reinterpret_cast<ModuleHookSpecification<static_cast<HookID>(ids)> *>(
         other.data() + ids * SpecificationSize
       ),
       1,
       reinterpret_cast<ModuleHookSpecification<static_cast<HookID>(ids)> *>(
         this->hooks.data() + ids * SpecificationSize
       )
     ),
     ...);
  }

  template <typename T, T... ids> void destructor_helper(std::integer_sequence<T, ids...>) {
    (std::destroy_at(
       reinterpret_cast<ModuleHookSpecification<static_cast<HookID>(ids)> *>(
         this->hooks.data() + ids * SpecificationSize
       )
     ),
     ...);
  }

public:
  std::array<uint8_t, SpecificationSize * HookCount> hooks;

  ModuleHooks();
  ModuleHooks(const ModuleHooks &) = delete;
  ModuleHooks(ModuleHooks &&other) noexcept;
  auto operator=(const ModuleHooks &) -> ModuleHooks &          = delete;
  auto operator=(ModuleHooks &&other) noexcept -> ModuleHooks & = delete;
  ~ModuleHooks();

  template <HookID hook_id> auto get() -> ModuleHookSpecification<hook_id> & {
    return *reinterpret_cast<ModuleHookSpecification<hook_id> *>(
      this->hooks.data() + SpecificationSize * static_cast<unsigned int>(hook_id)
    );
  }
};

// information about protocols implemented by the module
struct ModuleProtocols {
  // name of protocols this module implements
  std::span<std::string_view> protocols;

  // dispatcher for protocol requests
  protocol_handler_t accepter;
};

// attributes that indicates the property of modules
enum ModuleAttribute : uint8_t {
  Empty           = 0x0u,
  // this module should be kept alive, which means that this module have code that runs after the program
  //  is launched. In practice, this means that glaunch shall not do a direct execute, but fork before
  //  executing the program to be launched to give this module change to work as expected
  KeepAlive       = 0x1u,
  // this module implements at least one protocol
  ProtocolHandler = 0x2u,
};

// full specification of a module used to register it to module manager
struct ModuleRegistrationTable {
  // name of the module, which must not start with '$'
  std::string_view name;

  // hooks implemented by the module
  ModuleHooks hooks;

  // protocols implemented by the module
  ModuleProtocols protocols;

  // attributes of the module
  std::underlying_type_t<ModuleAttribute> attributes;

  // arbitrary pointer, the module may save anything within it
  void                       *module_data;
  // destructor, which will be called with the module_data
  std::function<void(void *)> destructor;
};

// this block actually controls nothing: all it does is holding very basic information, the name and attribute
//  of each registered module, with the arbitrary pointer and destructor supplied in the registration table,
//  which will be called when the manager destructs with the pointer as its only parameter
struct ModuleControlBlock {
  std::string_view                        name;
  std::underlying_type_t<ModuleAttribute> attributes;
  void                                   *module_data;
  std::function<void(void *)>             destructor;

  ModuleControlBlock(
    std::string_view                        name,
    std::underlying_type_t<ModuleAttribute> attributes,
    void                                   *module_data,
    std::function<void(void *)>           &&destructor
  );
};

// the module manager
//  this class cannot be initialized directly but must be initialized via the GLaunchModuleManagerBuilder
//  see GLaunchModuleManagerBuilder for more explanation
class GlaunchModuleManager {
public:
  // dispatch and forward calls to standard protocols
  //  this method is recommended for any invocations to standard protocol by referring them with symbolic name
  //   defined in glaunch-protocol.hh, since type check to the arguments may be applied at compile time, and
  //   the returned result is well-typed instead of contained in std::any
  template <protocol_id_t protocol_id, typename... Args>
  auto handle_request(Args &&...args) const -> standard_protocol_batched_result_t<protocol_id> {
    static_assert(
      is_standard_protocol<protocol_id>, "this dispatcher can only be used on standard protocols."
    );
    static_assert(
      std::invocable<standard_protocol_callable_t<protocol_id>, Args...>,
      "arguments mismatched with the protocol definition."
    );
    auto result = this->handle_request(protocol_id, std::forward<Args>(args)...);
    standard_protocol_batched_result_t<protocol_id> typed_result;
    std::ranges::transform(result, std::back_inserter(typed_result), [](const std::any &erased_result) {
      return std::any_cast<standard_protocol_result_t<protocol_id>>(erased_result);
    });
    return typed_result;
  }
  // dispatch and forward calls to protocols
  //  no type checking is applied at compile time and the results are contained in std::any
  //  if you are calling standard protocols, try to use the other version which accepts the protocol id as
  //   a template argument
  template <typename... Args>
  auto handle_request(std::string_view protocol, Args &&...args) const
    -> protocol_result_batcher_t<std::any> {
    return this->forward_requests(protocol, std::make_any(std::forward_as_tuple(args...)));
  }

  template <HookID hook_id, typename... Args>
  void execute_hook(Args &&...args, bool preserve = false) const
    requires std::invocable<hook_callable_t<hook_id>, Args...>
  {
    for (const auto index : this->hook_ranges[static_cast<size_t>(hook_id)]) {
      (reinterpret_cast<hook_callable_t<hook_id> *>(this->hooks)[index])(std::forward<Args>(args)...);
    }
    if (!preserve) {
      this->free_hooks<hook_id>();
    }
  }

  ~GlaunchModuleManager();

private:
  GlaunchModuleManager(
    std::unique_ptr<std::vector<uint8_t>>                                 &&internal_structures,
    std::array<Internal::range, HookCount>                                  hook_ranges,
    std::function<void()>                                                  *hooks,
    std::unordered_map<std::string_view, std::span<protocol_handler_t *>> &&accepter,
    std::span<ModuleControlBlock>                                           modules
  );
  // do dispatch of protocol calls
  [[nodiscard]] auto forward_requests(std::string_view protocol, const std::any &args) const
    -> protocol_result_batcher_t<std::any>;

  // memory block for internal structures
  //  Since the internal structure will be fixed once built, using containers like std::vector will introduce
  //  extra memory cost for dynamic insertion support, which will never be used. std::array would be fine, but
  //  the number of elements cannot be determined at compile time.
  //  Therefore, we use a memory block, which is allocated when the manager is allocated, to store most of the
  //  internal structures in a linear way, and use std::span to group them. Since some of the hooks have fixed
  //  and strong sequencial relationship and can be freed safely after certain point, we use std::vector to
  //  serve as the memory block which enables the freeing. We use std::unique_ptr to protect the memory block
  //  from being corrupted, since std::function instances are not TriviallyCopyable.
  //  See implementation of GLaunchModuleManagerBuilder::finalize for more detail about the order of elements
  //  saved in the memory block.
  //  It is proved that it is impossible to order accepter functions each implementing an arbitrary set of
  //  protocols into a linear sequence in which functions implementing each protocol forms a contiguous
  //  subsequence. Therefore, we store the instances of std::function in the memory block and reference them
  //  with raw points stored in std::forward_list.
  //
  //  std::vector introduces 24 bytes of memory overhead due to the metadata it must hold with no overhead on
  //   the heap after shrink, which is 24 * number of lists in total;
  //  std::forward_list introduces 8 byte of metadata with at least 8 byte of overhead for each element stored
  //   on the heap, which is 8 * number of lists + 8 * number of elements in total;
  //  std::span holds 16 bytes of metadata. Note that the memory block is shared amongst all lists, therefore
  //   it is only 16 * number of lists + 32 here in total.
  std::unique_ptr<std::vector<uint8_t>> internal_structures;

  // ordered list of hook: they should be called in order
  std::array<Internal::range, HookCount> hook_ranges;
  std::function<void()>                 *hooks;

  // accepter grouped by the protocol they implemented
  std::unordered_map<std::string_view, std::span<protocol_handler_t *>> accepter;

  // these specializations of std::function must share the same size
  static_assert(sizeof(hook_callable_t<static_cast<HookID>(0)>) == sizeof(protocol_handler_t));

  // control blocks that actually controls nothing
  //  be careful: the blocks can be in any order
  std::span<ModuleControlBlock> modules;

  // helper to reduce size of memory block (the internal structures)
  void free_memory(size_t reduce_by);

  // implementation of free hooks: free a single hook
  template <HookID hook_id> void free_single_hook(bool shrink_memory = true) {
    auto &range = this->hook_ranges[static_cast<size_t>(hook_id)];
    // no-op if it is an empty range
    if (range.size() == 0) {
      return;
    }
    // call destructors on them
    std::destroy(
      reinterpret_cast<hook_callable_t<hook_id> *>(hooks) + range.start_,
      reinterpret_cast<hook_callable_t<hook_id> *>(hooks) + range.end_
    );
    // reduce the size of memory block if required
    if (shrink_memory) {
      this->free_memory(HookCallableSize * range.size());
    }
    // reset the range so that mistakenly access such range again will just result in no-op
    range.start_ = 0;
    range.end_   = 0;
  }

  // helper to free and remove the sub memory block used to hold callables of certain hook
  //  note that the order is preserved: the hooks will be freed in strictly the same order as specified
  template <HookID... hook_id> void free_hooks(bool shrink_memory = true) {
    (..., this->free_single_hook<hook_id>(shrink_memory));
  }
  template <typename T, T... value>
  void free_hooks(std::integer_sequence<T, value...>, bool shrink_memory = true) {
    this->free_hooks<static_cast<HookID>(value)...>(shrink_memory);
  }

  friend class GLaunchModuleManagerBuilder;
};

// builder of module manager to which modules can register themselves with ModuleRegistrationTable
// after all registration have been done, finalize should be called on this class to acquire the manager
// this is designed to make sure that no further registration is possible after the manager has beed finalized
//  and the manager cannot be used to invoke hooks before being finalized since internal structures must be
//  built to do that and rebuilding is required for each new module registered
class GLaunchModuleManagerBuilder {
public:
  // register a module
  void register_module(ModuleRegistrationTable &&registration_table);

  // finalize the registration and build a module manager for modules registered
  auto finalize() -> GlaunchModuleManager;

private:
  std::unordered_map<std::string_view, ModuleRegistrationTable> registration_tables;
};
} // namespace GPS
#endif