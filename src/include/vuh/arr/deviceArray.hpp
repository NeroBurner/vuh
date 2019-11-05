#pragma once

#include "vuh/core/core.hpp"
#include "vuh/mem/memProperties.h"
#include "arrayIter.hpp"
#include "arrayUtils.h"
#include "vuh/mem/allocDevice.hpp"
#include "basicArray.hpp"
#include "hostArray.hpp"
#include <vuh/traits.hpp>
#include <algorithm>
#include <cassert>

namespace vuh {
namespace arr {

/// Array class not supposed to take part in data exchange with host.
/// The only valid use for such arrays is passing them as (in or out) argument to a shader.
/// Resources allocation is handled by allocator defined by a template parameter which is supposed to provide
/// memory and underlying vulkan buffer with suitable flags.
template<class T, class Alloc>
class DeviceOnlyArray: public BasicArray<T, Alloc> {
public:
	using value_type = T;
   /// Constructs object of the class on given device.
   /// Memory is left unintitialized.
   DeviceOnlyArray( vuh::Device& dev  ///< deice to create array on
	               , size_t n_elements    ///< number of elements
	               , vhn::MemoryPropertyFlags flags_mem={} ///< additional (to defined by allocator) memory usage flags
	               , vhn::BufferUsageFlags flags_buffer={})   ///< additional (to defined by allocator) buffer usage flags
	   : BasicArray<T, Alloc>(dev, n_elements, flags_mem, flags_buffer)
	{}
}; // class DeviceOnlyArray

/// Array with host data exchange interface suitable for memory allocated in device-local space.
/// Memory allocation and underlying buffer creation is managed by allocator defined by a template parameter.
/// Such allocator is expected to allocate memory in device local memory not mappable
/// for host access. However actual allocation may take place in a host-visible memory.
/// Some functions (like toHost(), fromHost()) switch to using the simplified data exchange methods
/// in that case. Some do not. In case all memory is host-visible (like on integrated GPUs) using this class
/// may result in performance penalty.
template<class T, class Alloc>
class DeviceArray: public BasicArray<T, Alloc> {
	using Base = BasicArray<T, Alloc>;
public:
	using value_type = T;
	/// Create an instance of DeviceArray with given number of elements. Memory is uninitialized.
	DeviceArray( vuh::Device& device   ///< device to create array on
	           , size_t n_elements     ///< number of elements
	           , vhn::MemoryPropertyFlags flags_mem={} ///< additional (to defined by allocator) memory usage flags
	           , vhn::BufferUsageFlags flags_buffer={})   ///< additional (to defined by allocator) buffer usage flags
	   : Base(device, n_elements, flags_mem, flags_buffer)
	{}

	/// Create an instance of DeviceArray and initialize memory by content of some host iterable.
	template<class C, class=typename std::enable_if_t<vuh::traits::is_iterable<C>::value>>
	DeviceArray(vuh::Device& dev  ///< device to create array on
	           , const C& c          ///< iterable to initialize from
	           , vhn::MemoryPropertyFlags flags_mem={} ///< additional (to defined by allocator) memory usage flags
	           , vhn::BufferUsageFlags flags_buffer={})	  ///< additional (to defined by allocator) buffer usage flags
	   : DeviceArray(dev, c.size()/sizeof(T), flags_mem, flags_buffer)
	{
		using std::begin; using std::end;
		fromHost(begin(c), end(c));
	}

	/// Create an instance of DeviceArray and initialize it from a range of values.
	template<class It1, class It2>
   DeviceArray(vuh::Device& dev   ///< device to create array on
	            , It1 begin           ///< range begin
	            , It2 end             ///< range end (points to one past the last element of the range)
	            , vhn::MemoryPropertyFlags flags_mem={} ///< additional (to defined by allocator) memory usage flags
	            , vhn::BufferUsageFlags flags_buffer={})	///< additional (to defined by allocator) buffer usage flags
	   : DeviceArray(dev, std::distance(begin, end)/sizeof(T), flags_mem, flags_buffer)
	{
		fromHost(begin, end);
	}

	/// Create an instance of DeviceArray of given size and initialize it using index based initializer function.
	template<class F>
	DeviceArray( vuh::Device& dev  ///< device to create array on
	           , size_t n_elements    ///< number of elements
	           , F&& fun              ///< callable of a form function<T(size_t)> mapping an offset to array value
	           , vhn::MemoryPropertyFlags flags_mem={} ///< additional (to defined by allocator) memory usage flags
	           , vhn::BufferUsageFlags flags_buffer={})	  ///< additional (to defined by allocator) buffer usage flags
	   : DeviceArray(dev, n_elements, flags_mem, flags_buffer)
	{
		using std::begin;
		auto stage_buffer = HostArray<T, vuh::mem::AllocDevice<vuh::mem::properties::HostCoherent>>(Base::_dev, n_elements);
		auto stage_it = begin(stage_buffer);
		for(size_t i = 0; i < n_elements; ++i, ++stage_it){
			*stage_it = fun(i);
		}
		copyBuf(Base::_dev, stage_buffer, *this, Base::size_bytes());
	}
   
	/// Copy data from host range to array memory.
	template<class It1, class It2>
	auto fromHost(It1 begin, It2 end)-> void {
		if(Base::isHostVisible()){
			std::copy(begin, end, host_data());
			Base::_dev.unmapMemory(Base::_mem);
		} else { // memory is not host visible, use staging buffer
			auto stage_buf = HostArray<T, vuh::mem::AllocDevice<vuh::mem::properties::HostCoherent>>(Base::_dev, begin, end);
			copyBuf(Base::_dev, stage_buf, *this, Base::size_bytes());
		}
	}
   
	/// Copy data from host range to array memory with offset
	template<class It1, class It2>
	auto fromHost(It1 begin, It2 end, size_t offset)-> void {
		if(Base::isHostVisible()){
			std::copy(begin, end, host_data() + offset);
			Base::_dev.unmapMemory(Base::_mem);
		} else { // memory is not host visible, use staging buffer
			auto stage_buf = HostArray<T, vuh::mem::AllocDevice<vuh::mem::properties::HostCoherent>>(Base::_dev, begin, end);
			copyBuf(Base::_dev, stage_buf, *this, Base::size_bytes(), 0u, offset*sizeof(T));
		}
	}

   /// Copy data from array memory to host location indicated by iterator.
	/// The whole array data is copied over.
   template<class It>
   auto toHost(It copy_to) const-> void {
      if(Base::isHostVisible()){
         std::copy_n(host_data(), Base::size(), copy_to);
         Base::_dev.unmapMemory(Base::_mem);
      } else {
         using std::begin; using std::end;
         auto stage_buf = HostArray<T, vuh::mem::AllocDevice<vuh::mem::properties::HostCached>>(Base::_dev, Base::size());
         copyBuf(Base::_dev, *this, stage_buf, Base::size_bytes());
         std::copy(begin(stage_buf), end(stage_buf), copy_to);
      }
   }
   
   /// Copy-transform data from array memory to host location indicated by iterator.
	/// The whole array data is transformed.
   template<class It, class F>
   auto toHost(It copy_to, F&& fun) const-> void {
      if(Base::isHostVisible()){
         auto copy_from = host_data();
         std::transform(copy_from, copy_from + Base::size(), copy_to, std::forward<F>(fun));
         Base::_dev.unmapMemory(Base::_mem);
      } else {
         using std::begin; using std::end;
         auto stage_buf = HostArray<T, vuh::mem::AllocDevice<vuh::mem::properties::HostCached>>(Base::_dev, Base::size());
         copyBuf(Base::_dev, *this, stage_buf, Base::size_bytes());
         std::transform(begin(stage_buf), end(stage_buf), copy_to, std::forward<F>(fun));
      }
   }
   
   /// Copy-transform the chunk of data of given size from the beginning of array.
   template<class It, class F>
   auto toHost( It copy_to  ///< iterator indicating starting position to write to
	           , size_t size ///< number of elements to transform
	           , F&& fun     ///< transform function
	           ) const-> void
	{
		if(Base::isHostVisible()){
			auto copy_from = host_data();
			std::transform(copy_from, copy_from + size, copy_to, std::forward<F>(fun));
			Base::_dev.unmapMemory(Base::_mem);
		} else {
			using std::begin; using std::end;
			auto stage_buf = HostArray<T, vuh::mem::AllocDevice<vuh::mem::properties::HostCached>>(Base::_dev, size);
			copyBuf(Base::_dev, *this, stage_buf, Base::size_bytes());
			std::transform(begin(stage_buf), end(stage_buf), copy_to, std::forward<F>(fun));
		}
	}

	/// Copy range of values from device to host memory.
	template<class DstIter>
	auto rangeToHost(size_t offset_begin, size_t offset_end, DstIter dst_begin) const-> void {
		if(Base::isHostVisible()){
			auto copy_from = host_data();
			std::copy(copy_from + offset_begin, copy_from + offset_end, dst_begin);
			Base::_dev.unmapMemory(Base::_mem);
		} else {
			using std::begin; using std::end;
			auto stage_buf = HostArray<T, vuh::mem::AllocDevice<vuh::mem::properties::HostCached>>(Base::_dev
			                                                          , offset_end - offset_begin);
			copyBuf(Base::_dev, *this, stage_buf, Base::size_bytes(), offset_begin, 0u);
			std::copy(begin(stage_buf), end(stage_buf), dst_begin);
		}
	}
	
	/// @return host container with a copy of array data.
	template<class C, typename=typename std::enable_if_t<vuh::traits::is_iterable<C>::value>>
	auto toHost() const-> C {
		auto ret = C(Base::size());
		using std::begin;
		toHost(begin(ret));
		return ret;
	}

	/// doc me
	auto device_begin()-> ArrayIter<DeviceArray> { return ArrayIter<DeviceArray>(*this, 0); }
	auto device_begin() const-> ArrayIter<DeviceArray> { return ArrayIter<DeviceArray>(*this, 0); }

	/// doc me
	auto device_end()-> ArrayIter<DeviceArray> {return ArrayIter<DeviceArray>(*this, Base::size());}
	auto device_end() const-> ArrayIter<DeviceArray> {return ArrayIter<DeviceArray>(*this, Base::size());}
private: // helpers
	auto host_data()-> T* {
		assert(Base::isHostVisible());
        auto data = Base::_dev.mapMemory(Base::_mem, 0, Base::size_bytes());
#ifdef VULKAN_HPP_NO_EXCEPTIONS
        VULKAN_HPP_ASSERT(vhn::Result::eSuccess == data.result);
        if (vhn::Result::eSuccess == data.result) {
            return static_cast<T *>(data.value);
        }
        return nullptr;
#else
		return static_cast<T*>(data);
#endif
	}

	auto host_data() const-> const T* {
		assert(Base::isHostVisible());
        auto data = Base::_dev.mapMemory(Base::_mem, 0, Base::size_bytes());
#ifdef VULKAN_HPP_NO_EXCEPTIONS
        VULKAN_HPP_ASSERT(vhn::Result::eSuccess == data.result);
		if (vhn::Result::eSuccess == data.result) {
            return static_cast<const T *>(data.value);
        }
        return nullptr;
#else
        return static_cast<const T*>(data);
#endif
	}
}; // class DeviceArray

/// doc me
template<class T, class Alloc>
auto device_begin(DeviceArray<T, Alloc>& array)-> ArrayIter<DeviceArray<T, Alloc>> {
	return array.device_begin();
}

/// doc me
template<class T, class Alloc>
auto device_end(DeviceArray<T, Alloc>& array)-> ArrayIter<DeviceArray<T, Alloc>> {
	return array.device_end();
}

} // namespace arr
} // namespace vuh
