///////////////////////////////////////////////////////////////////////////////
// Copyright (c) Lewis Baker
// Licenced under MIT license. See LICENSE.txt for details.
///////////////////////////////////////////////////////////////////////////////
#ifndef CPPCORO_DETAIL_LINUX_HPP_INCLUDED
#define CPPCORO_DETAIL_LINUX_HPP_INCLUDED

#include <cppcoro/config.hpp>

#include <cstdint>
#include <cstdio>
#include <utility>

#include <fcntl.h>
#include <mqueue.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <ctime>
#include <unistd.h>
#include <utility>

#include <string_view>
#include <mutex>
#include <cppcoro/coroutine.hpp>
#include <functional>

namespace cppcoro
{
	namespace detail
	{
		namespace lnx
		{
			using fd_t = int;

			class safe_fd
			{
			public:
				safe_fd()
					: m_fd(-1)
				{
				}

				explicit safe_fd(fd_t fd)
					: m_fd(fd)
				{
				}

				safe_fd(const safe_fd& other) = delete;

				safe_fd(safe_fd&& other) noexcept
					: m_fd(other.m_fd)
				{
					other.m_fd = -1;
				}

				~safe_fd() { close(); }

				safe_fd& operator=(safe_fd fd) noexcept
				{
					swap(fd);
					return *this;
				}

				constexpr fd_t fd() const { return m_fd; }
                constexpr fd_t handle() const { return m_fd; }

				/// Calls close() and sets the fd to -1.
				void close() noexcept;

				void swap(safe_fd& other) noexcept { std::swap(m_fd, other.m_fd); }

				/// Test operator
				explicit operator bool() noexcept {
					return m_fd >= 0;
				}

				/// Dereference operator
				int operator*() const noexcept {
					return fd();
				}

				bool operator==(const safe_fd& other) const { return m_fd == other.m_fd; }

				bool operator!=(const safe_fd& other) const { return m_fd != other.m_fd; }

				bool operator==(fd_t fd) const { return m_fd == fd; }

				bool operator!=(fd_t fd) const { return m_fd != fd; }

			private:
				fd_t m_fd;
			};

			struct io_message
			{
				std::function<void()> resume;
				int   result = -1;

				io_message& operator=(coroutine_handle<> coroutine_handle) noexcept {
					resume = [coroutine_handle]() mutable {
						coroutine_handle.resume();
					};
                    return *this;
				}
                io_message& operator=(std::function<void()> function) noexcept {
                    resume = std::move(function);
                    return *this;
                }
			};

		}  // namespace linux

		using safe_handle = lnx::safe_fd;
        using dword_t = int;
		struct sock_buf {
            sock_buf(void *buf, size_t sz) : buffer(buf), size(sz) {}
			void * buffer;
			size_t size;
		};
		using handle_t = lnx::fd_t;
    }  // namespace detail
}  // namespace cppcoro

#endif // CPPCORO_DETAIL_LINUX_HPP_INCLUDED
