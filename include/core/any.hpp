#ifndef CORE_ANY_HPP
#define CORE_ANY_HPP

#include <type_traits>
#include <stdexcept>
#include <typeinfo>
#include <memory>

namespace core {
inline namespace v1 {
namespace impl {

template <typename Type>
using is_small = std::integral_constant<bool, sizeof(Type) <= sizeof(void*)>;

template <typename Type>
using decay = typename std::decay<Type>::type;


struct any_dispatch {
  using destroy_function = void (*)(void**);
  using clone_function = void(*)(void* const*, void**);
  using type_function = std::type_info const& (*)();

  destroy_function const destroy;
  clone_function const clone;
  type_function const type;
};

template <typename Type, bool=is_small<Type>::value>
struct any_dispatch_select;

template <typename Type>
struct any_dispatch_select<Type, true> {
  using allocator_type = std::allocator<Type>;

  static void clone (void* const* source, void** data) {
    allocator_type alloc { };
    auto const& value = *reinterpret_cast<Type const*>(source);
    auto pointer = reinterpret_cast<Type*>(data);
    std::allocator_traits<allocator_type>::construct(alloc, pointer, value);
  }

  static void destroy (void** data) {
    allocator_type alloc { };
    auto pointer = reinterpret_cast<Type*>(data);
    std::allocator_traits<allocator_type>::destroy(alloc, pointer);
  }
};

template <typename Type>
struct any_dispatch_select<Type, false> {
  using allocator_type = std::allocator<Type>;

  static void clone (void* const* source, void** data) {
    allocator_type alloc { };
    auto const& value = **reinterpret_cast<Type* const*>(source);
    auto pointer = std::allocator_traits<allocator_type>::allocate(alloc, 1);
    std::allocator_traits<allocator_type>::construct(alloc, pointer, value);
    *data = pointer;
  }

  static void destroy (void** data) {
    allocator_type alloc { };
    auto& value = *reinterpret_cast<Type**>(data);
    std::allocator_traits<allocator_type>::destroy(alloc, value);
    std::allocator_traits<allocator_type>::deallocate(alloc, value, 1);
  }
};

template <typename Type>
any_dispatch const* get_any_dispatch () {
  static any_dispatch const instance = {
    any_dispatch_select<Type>::destroy,
    any_dispatch_select<Type>::clone,
    [] () -> std::type_info const& { return typeid(Type); }
  };
  return std::addressof(instance);
}

template <>
inline any_dispatch const* get_any_dispatch<void> () {
  static any_dispatch const instance = {
    [] (void**) { },
    [] (void* const*, void**) { },
    [] () -> std::type_info const& { return typeid(void); }
  };
  return std::addressof(instance);
}

} /* namespace impl */

class bad_any_cast final : public std::bad_cast {
public:
  virtual char const* what () const noexcept override {
    return "bad any cast";
  }
};

class any final {
  template <typename ValueType> friend ValueType any_cast (any const&);
  template <typename ValueType> friend ValueType any_cast (any&);

  template <typename ValueType>
  friend ValueType const* any_cast (any const*) noexcept;
  template <typename ValueType> friend ValueType* any_cast (any*) noexcept;

  impl::any_dispatch const* table;
  void* data;

  template <typename ValueType>
  any (ValueType&& value, std::true_type&&) :
    table { impl::get_any_dispatch<impl::decay<ValueType>>() },
    data { nullptr }
  {
    using value_type = impl::decay<ValueType>;
    using allocator_type = std::allocator<value_type>;
    allocator_type alloc { };
    auto pointer = reinterpret_cast<value_type*>(std::addressof(this->data));
    std::allocator_traits<allocator_type>::construct(
      alloc, pointer, std::forward<ValueType>(value)
    );
  }

  template <typename ValueType>
  any (ValueType&& value, std::false_type&&) :
    table { impl::get_any_dispatch<impl::decay<ValueType>>() },
    data { nullptr }
  {
    using value_type = impl::decay<ValueType>;
    using allocator_type = std::allocator<value_type>;
    allocator_type alloc { };
    auto pointer = std::allocator_traits<allocator_type>::allocate(alloc, 1);
    std::allocator_traits<allocator_type>::construct(
      alloc, pointer, std::forward<ValueType>(value)
    );
    this->data = pointer;
  }

  template <typename ValueType>
  ValueType const* cast (std::true_type&&) const {
    return reinterpret_cast<ValueType const*>(std::addressof(this->data));
  }

  template <typename ValueType>
  ValueType* cast (std::true_type&&) {
    return reinterpret_cast<ValueType*>(std::addressof(this->data));
  }

  template <typename ValueType>
  ValueType const* cast (std::false_type&&) const {
    return reinterpret_cast<ValueType const*>(this->data);
  }

  template <typename ValueType>
  ValueType* cast (std::false_type&&) {
    return reinterpret_cast<ValueType*>(this->data);
  }

public:
  any (any const& that) :
    table { that.table },
    data { nullptr }
  {
    this->table->clone(std::addressof(that.data), std::addressof(this->data));
  }

  any (any&& that) noexcept :
    table { that.table },
    data { that.data }
  {
    that.table = impl::get_any_dispatch<void>();
    that.data = nullptr;
  }

  any () noexcept :
    table { impl::get_any_dispatch<void>() },
    data { nullptr }
  { }

  template <
    typename ValueType,
    typename=typename std::enable_if<
      not std::is_same<any, impl::decay<ValueType>>::value
    >::type
  > any (ValueType&& value) :
    any { std::forward<ValueType>(value), impl::is_small<ValueType> { } }
  { }

  ~any () noexcept { this->table->destroy(std::addressof(this->data)); }

  any& operator = (any const& that) {
    any { that }.swap(*this);
    return *this;
  }

  any& operator = (any&& that) noexcept {
    any { std::move(that) }.swap(*this);
    return *this;
  }

  void swap (any& that) noexcept {
    std::swap(this->table, that.table);
    std::swap(this->data, that.data);
  }

  std::type_info const& type () const noexcept { return this->table->type(); }

  bool empty () const noexcept {
    return this->table == impl::get_any_dispatch<void>();
  }

};

template <typename ValueType>
ValueType any_cast (any const& operand) {
  using type = typename::std::remove_reference<ValueType>::type;
  using return_type = typename std::add_const<type>::type;
  if (operand.type() != typeid(type)) { throw bad_any_cast { }; }
  return *operand.cast<return_type>(impl::is_small<ValueType> { });
}

template <typename ValueType>
ValueType any_cast (any& operand) {
  using return_type = typename std::remove_reference<ValueType>::type;
  if (operand.type() != typeid(return_type)) { throw bad_any_cast { }; }
  return *operand.cast<return_type>(impl::is_small<ValueType> { });
}

template <typename ValueType>
ValueType const* any_cast (any const* operand) noexcept {
  return operand and operand->type() == typeid(ValueType)
    ? operand->cast<ValueType>(impl::is_small<ValueType> { })
    : nullptr;
}

template <typename ValueType>
ValueType* any_cast (any* operand) noexcept {
  return operand and operand->type() == typeid(ValueType)
    ? operand->cast<ValueType>(impl::is_small<ValueType> { })
    : nullptr;
}

}} /* namespace core::v1 */

namespace std {

inline void swap (core::any& lhs, core::any& rhs) noexcept { lhs.swap(rhs); }

} /* namespace std */

#endif /* CORE_ANY_HPP */