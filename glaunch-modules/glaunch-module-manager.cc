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

#include "glaunch-hooks.hh"
#include <algorithm>
#include <functional>
#include <glaunch-module-manager.hh>

#include <cassert>
#include <cstdlib>
#include <memory>
#include <print>
#include <queue>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
struct OrderingPack {
  // name of modules that this callable will run before
  std::unordered_set<std::string_view> before;
  // number of modules that must run before this callable (remaining)
  size_t                               after;
};

// find the ordering package with the specified expression
//  returns the name and ordering package
static auto target_finder(
  std::unordered_map<std::string_view, OrderingPack> &ordering,
  const std::string_view                              expression,
  uint8_t                                             hook_id,
  const std::string_view                              source_name,
  const char                                         *relationship
) -> std::pair<std::string_view, OrderingPack &> {
  const auto required = expression.ends_with('!');
  const auto name     = required ? expression.substr(0, expression.size() - 1) : expression;
  auto       iter     = ordering.find(name);
  if (iter == ordering.end()) {
    std::println(
      stderr,
      "module {} requests to execute its hook {} {} module {}, which does not exist",
      source_name,
      hook_id,
      relationship,
      name
    );
    if (required) {
      exit(EXIT_FAILURE);
    }
    return {std::string_view{}, ordering.begin()->second};
  }
  return {name, iter->second};
};

// insert ordering relationship: first should be executed before second
static void
insert_ordering(OrderingPack &first_pack, std::string_view second_name, OrderingPack &second_pack) {
  first_pack.before.emplace(second_name);
  second_pack.after++;
}

// detect loop in the dependency graph which indicates that the module cannot be executed in some order
//  that satisfies the ordering requirements of all modules
// this function returns an empty string if nothing wrong is detected, or the name of first module detected
//  in a loop
static auto detect_loop(
  const std::unordered_map<std::string_view, OrderingPack> &ordering,
  const std::string_view                                    current_name,
  const OrderingPack                                       &current_pack,
  std::unordered_set<std::string_view>                     &in_stack,
  std::unordered_set<std::string_view>                     &visited
) -> std::string_view {
  if (visited.contains(current_name)) {
    return {};
  }
  visited.emplace(current_name);
  in_stack.emplace(current_name);
  for (const auto next : current_pack.before) {
    if (in_stack.contains(next)) {
      std::print(stderr, "loop in module dependency graph detected!\n  {} --> {}", next, current_name);
      return next;
    }
    const auto result = detect_loop(ordering, next, ordering.at(next), in_stack, visited);
    if (!result.empty()) {
      // loop including current module has been detected, dump information about it
      std::print(stderr, " --> {}", current_name);
      if (result != current_name) {
        // the full loop has not yet been dumped, continue
        return result;
      }
      // this is the last module in the loop, summary and exit
      std::println(stderr, "( --> <to the beginning>)");
      exit(EXIT_FAILURE);
    }
  }
  in_stack.erase(current_name);
  return {};
}

template <GPS::HookID hook_id>
static auto
build_hooks_list(std::unordered_map<std::string_view, GPS::ModuleRegistrationTable> &registration_tables)
  -> std::vector<GPS::hook_callable_t<hook_id>> {
  std::unordered_map<std::string_view, OrderingPack> ordering;
  for (const auto &[_, table] : registration_tables) {
    ordering.emplace(table.name, OrderingPack{});
  }

  // build the graph
  for (auto &[_, table] : registration_tables) {
    auto       &this_ordering = ordering.at(table.name);
    const auto &specification = table.hooks.get<hook_id>();
    for (const auto expression : specification.after) {
      auto [name, other_ordering] = target_finder(
        ordering, expression, static_cast<std::underlying_type_t<GPS::HookID>>(hook_id), table.name, "after"
      );
      if (name.empty()) {
        continue;
      }
      insert_ordering(other_ordering, table.name, this_ordering);
    }
    for (const auto expression : specification.before) {
      auto [name, other_ordering] = target_finder(
        ordering, expression, static_cast<std::underlying_type_t<GPS::HookID>>(hook_id), table.name, "before"
      );
      if (name.empty()) {
        continue;
      }
      insert_ordering(this_ordering, name, other_ordering);
    }
  }

  // check for loops
  {
    std::unordered_set<std::string_view> in_stack;
    std::unordered_set<std::string_view> visited;
    for (const auto &[name, order] : ordering) {
      detect_loop(ordering, name, order, in_stack, visited);
    }
  }

  // build the sequence
  std::vector<GPS::hook_callable_t<hook_id>> result;
  std::queue<std::string_view>               pending_modules;
  for (const auto &[name, order] : ordering) {
    if (order.after == 0) {
      pending_modules.push(name);
    }
  }
  while (!pending_modules.empty()) {
    const auto name = pending_modules.front();
    pending_modules.pop();
    const auto &order = ordering.at(name);
    auto       &table = registration_tables.at(name);

    auto &specification = table.hooks.get<hook_id>();
    if (specification.callable) { // do not include empty functions
      result.emplace_back(std::move(specification.callable));
      assert(!specification.callable);
    }

    for (const auto before : order.before) {
      auto &next_order = ordering.at(before);
      if (--(next_order.after) == 0) {
        pending_modules.push(before);
      }
    }
  }
  return result;
}
template <typename T> auto start_point_helper(uint8_t *&current_start_point, size_t elements) -> T * {
  auto result = reinterpret_cast<T *>(current_start_point);
  current_start_point += elements * sizeof(T); // NOLINT(bugprone-sizeof-expression)
  return result;
}
template <GPS::HookID hook_id>
auto fill_hook_callable(
  uint8_t *&start_point, uint16_t &filled_callables, std::vector<GPS::hook_callable_t<hook_id>> &callables
) -> GPS::Internal::range {
  auto start = start_point_helper<GPS::hook_callable_t<hook_id>>(start_point, callables.size());
  std::uninitialized_move(callables.begin(), callables.end(), start);
  GPS::Internal::range result{
    filled_callables, static_cast<GPS::Internal::range::size_type>(filled_callables + callables.size())
  };
  filled_callables += callables.size();
  return result;
}
} // namespace

namespace GPS {
ModuleHooks::ModuleHooks() {
  this->initialize_helper(std::make_integer_sequence<std::underlying_type_t<HookID>, HookCount>{});
}
ModuleHooks::ModuleHooks(ModuleHooks &&other) noexcept {
  this->move_helper(other.hooks, std::make_integer_sequence<std::underlying_type_t<HookID>, HookCount>{});
}
ModuleHooks::~ModuleHooks() {
  this->destructor_helper(std::make_integer_sequence<std::underlying_type_t<HookID>, HookCount>{});
}

GlaunchModuleManager::GlaunchModuleManager(
  std::unique_ptr<std::vector<uint8_t>>                                 &&internal_structures,
  std::array<Internal::range, HookCount>                                  hook_ranges,
  std::function<void()>                                                  *hooks,
  std::unordered_map<std::string_view, std::span<protocol_handler_t *>> &&accepter,
  std::span<ModuleControlBlock>                                           modules
)
  : internal_structures(std::move(internal_structures)), hook_ranges(hook_ranges), hooks(hooks),
    accepter(std::move(accepter)), modules(modules) {}

[[nodiscard]] auto
GlaunchModuleManager::forward_requests(std::string_view protocol, const std::any &args) const
  -> protocol_result_batcher_t<std::any> {
  const auto iter = this->accepter.find(protocol);
  if (iter == this->accepter.cend()) {
    return {};
  }
  protocol_result_batcher_t<std::any> result;
  for (const auto callable : iter->second) {
    result.emplace_front((*callable)(protocol, args));
  }
  return result;
}

GlaunchModuleManager::~GlaunchModuleManager() {
  // free the memory block since no destructor on the saved callables will be called automatically
  //  since hooks that have already been free will have their corresponding range set, we can skip the check
  //  since we are not trying to reduce long-term runtime memory usage, we skip the free_memory calls
  this->free_hooks(std::make_index_sequence<HookCount>{}, false);

  // points (references to the protocol accepter) do not need to be freed: they allocates nothing

  // call destructors for accepter
  //  we have to count number of accepter first
  size_t accepter_count = 0;
  for (const auto &module : this->modules) {
    if (module.attributes & ModuleAttribute::ProtocolHandler) {
      ++accepter_count;
    }
  }
  //  destroy them
  std::destroy_n(
    reinterpret_cast<protocol_handler_t *>(this->internal_structures->data() + this->modules.size_bytes()),
    accepter_count
  );

  // give modules a change to deactivate
  for (const auto &module : this->modules) {
    if (module.destructor) {
      module.destructor(module.module_data);
    }
  }

  // call destructor for control blocks
  std::destroy_n(
    reinterpret_cast<ModuleControlBlock *>(this->internal_structures->data()), this->modules.size()
  );
}

void GlaunchModuleManager::free_memory(size_t reduce_by) {
  auto saved_base_address = this->internal_structures->data();
  this->internal_structures->resize(this->internal_structures->size() - reduce_by);
  this->internal_structures->shrink_to_fit();
  // make assertion that the underlying memory block is not moved
  //  since std::function is not TriviallyCopyable, they cannot be moved properly without knowing the exact
  //  typing information, and all callables are referenced by pointers to the memory block which will be
  //  invalidated if the block is moved.
  // if this error triggers, we will have to make this operation a no-op, or to seek for some other way
  assert(saved_base_address == this->internal_structures->data());
}

void GLaunchModuleManagerBuilder::register_module(ModuleRegistrationTable &&registration_table) {
  if (this->registration_tables.emplace(registration_table.name, registration_table).second == false) {
    std::println(stderr, "registering module with duplicated name {}", registration_table.name);
    exit(EXIT_FAILURE);
  }
}
auto GLaunchModuleManagerBuilder::finalize() -> GlaunchModuleManager {
  // note that the hooks have a clear timepoint for execution, therefore it is possible to free hooks once
  //  we are sure that it will never be invoked again. before_fork will always be invoked before before_exec
  //  and after_parent_fork, while after_exit only get executed after them, therefore we place after_exit
  //  closer to the beginning of memory block, before_fork at the ending and before_exec and after_parent_fork
  //  between them so they can be freed simply with resize and shrink_to_fit.
  // protocol accepter can be used at any moment, make it impossible to free them reliably. Therefore, they
  //  should be placed to the very beginning of the memory block.
  // we place control blocks before the protocol accepter since they should also live for the whole time

  // the memory block. Since instances of std::function are not TriviallyCopyable, we must fill the memory
  //  block after all functions has been collected so the memory required can be reserved beforehand.
  auto memory_block = std::make_unique<std::vector<uint8_t>>();

  // 1. To make insertion most efficient, we should collect protocols first
  std::vector<protocol_handler_t>                           accepter;
  std::unordered_map<std::string_view, std::vector<size_t>> protocol_mapper;
  size_t                                                    reference_count = 0;
  for (auto &[_, table] : this->registration_tables) {
    if ((table.attributes & ModuleAttribute::ProtocolHandler) == 0) {
      continue;
    }
    if (!table.protocols.accepter || table.protocols.protocols.empty()) {
      std::println(
        stderr,
        "module {} registered as a implementation of protocols with no accepter or protocol name supplied.",
        table.name
      );
      exit(EXIT_FAILURE);
    }
    for (const auto &protocol : table.protocols.protocols) {
      auto iter = protocol_mapper.emplace(protocol, std::vector<size_t>{}).first;
      iter->second.emplace_back(accepter.size());
      reference_count++;
    }
    accepter.emplace_back(std::move(table.protocols.accepter));
    assert(!table.protocols.accepter);
  }

  // 2. collect hooks
  auto after_exit_hooks        = build_hooks_list<HookID::AfterExit>(this->registration_tables);
  auto before_exec_hooks       = build_hooks_list<HookID::BeforeExec>(this->registration_tables);
  auto after_parent_fork_hooks = build_hooks_list<HookID::AfterParentFork>(this->registration_tables);
  auto before_fork_hooks       = build_hooks_list<HookID::BeforeFork>(this->registration_tables);

  // 3. now that have have everything ready, we may start building the memory block
  // content of the memory block is arranged as follows:
  // +-----------------------------------------------+  <-- lowest address (beginning of memory block)
  // |           control block for module 1          |
  // +-----------------------------------------------+
  // |           control block for module 2          |
  // +-----------------------------------------------+
  // |                                               |
  //                        ...
  // |                                               |
  // +-----------------------------------------------+
  // |           control block for module N          |
  // +-----------------------------------------------+
  // |    instances of std::function for accepter    |
  // +-----------------------------------------------+
  // |            std::function* pointers            |
  // |     referring to accepter for protocol 1      |
  // +-----------------------------------------------+
  // |            std::function* pointers            |
  // |     referring to accepter for protocol 2      |
  // +-----------------------------------------------+
  // |                                               |
  //                        ...
  // |                                               |
  // +-----------------------------------------------+
  // |            std::function* pointers            |
  // |     referring to accepter for protocol M      |
  // +-----------------------------------------------+
  // |           instances of std::function          |
  // |              for after_exit hooks             |
  // +-----------------------------------------------+
  // |           instances of std::function          |
  // |             for before_exec hooks             |
  // +-----------------------------------------------+
  // |           instances of std::function          |
  // |          for after_parent_fork hooks          |
  // +-----------------------------------------------+
  // |           instances of std::function          |
  // |             for before_fork hooks             |
  // +-----------------------------------------------+  <-- highest address (ending of memory block)

  // 3.1. first, we allocate memory for the memory block
  constexpr const auto control_block_size      = sizeof(ModuleControlBlock);
  constexpr const auto function_size           = sizeof(protocol_handler_t);
  constexpr const auto function_reference_size = sizeof(protocol_handler_t *);
  memory_block->resize(
    control_block_size * this->registration_tables.size() // control blocks
    + function_size * accepter.size()                     // accepter
    + function_reference_size * reference_count           // references to accepter
    + function_size * after_exit_hooks.size()             // hook after_exit
    + function_size * before_exec_hooks.size()            // hook before_exec
    + function_size * after_parent_fork_hooks.size()      // hook after_parent_fork
    + function_size * before_fork_hooks.size()            // hook before_fork
  );
  memory_block->shrink_to_fit();

  auto current_start_point = memory_block->data();

  // 3.2. fill control blocks
  auto current_control_block =
    start_point_helper<ModuleControlBlock>(current_start_point, this->registration_tables.size());
  std::span<ModuleControlBlock> control_blocks{current_control_block, this->registration_tables.size()};
  for (auto &[_, table] : this->registration_tables) {
    std::construct_at(
      current_control_block, table.name, table.attributes, table.module_data, std::move(table.destructor)
    );
    assert(!table.destructor);
    current_control_block++;
  }

  // 3.3. fill accepter
  auto accepter_array = start_point_helper<protocol_handler_t>(current_start_point, accepter.size());
  std::uninitialized_move(accepter.begin(), accepter.end(), accepter_array);

  // 3.4. fill references while building the lookup table
  auto accepter_references = start_point_helper<protocol_handler_t *>(current_start_point, reference_count);
  std::unordered_map<std::string_view, std::span<protocol_handler_t *>> built_accepter;
  for (const auto &[name, indices] : protocol_mapper) {
    built_accepter.emplace(name, std::span<protocol_handler_t *>{accepter_references, indices.size()});
    for (const auto index : indices) {
      *accepter_references = accepter_array + index;
      accepter_references++;
    }
  }
  assert(reinterpret_cast<uint8_t *>(accepter_references) == current_start_point);

  // 3.5. fill hook callables and prepare ranges
  Internal::range::size_type filled_callables = 0;
  auto                       hook_start = reinterpret_cast<std::function<void()> *>(current_start_point);

  auto after_exit =
    fill_hook_callable<HookID::AfterExit>(current_start_point, filled_callables, after_exit_hooks);
  auto before_exec =
    fill_hook_callable<HookID::BeforeExec>(current_start_point, filled_callables, before_exec_hooks);
  auto after_parent_fork = fill_hook_callable<HookID::AfterParentFork>(
    current_start_point, filled_callables, after_parent_fork_hooks
  );
  auto before_fork =
    fill_hook_callable<HookID::BeforeFork>(current_start_point, filled_callables, before_fork_hooks);

  // 3.6. build the array for ranges
  //  we do this in such way since range for hook x must be stored at the slot indexed by its ID
  //  setting it explicitly makes it less possible to make mistake here
  std::array<Internal::range, HookCount> hooks;
  hooks[static_cast<size_t>(HookID::AfterExit)]       = after_exit;
  hooks[static_cast<size_t>(HookID::BeforeExec)]      = before_exec;
  hooks[static_cast<size_t>(HookID::AfterParentFork)] = after_parent_fork;
  hooks[static_cast<size_t>(HookID::BeforeFork)]      = before_fork;

  // 3.7. all done, build the instance of manger
  return {std::move(memory_block), hooks, hook_start, std::move(built_accepter), control_blocks};
}
} // namespace GPS