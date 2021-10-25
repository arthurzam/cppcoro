///////////////////////////////////////////////////////////////////////////////
// Copyright (c) Lewis Baker
// Licenced under MIT license. See LICENSE.txt for details.
///////////////////////////////////////////////////////////////////////////////
#ifndef CPPCORO_DETAIL_LINUX_URING_QUEUE_HPP_INCLUDED
#define CPPCORO_DETAIL_LINUX_URING_QUEUE_HPP_INCLUDED

#include <cppcoro/config.hpp>

#include <cppcoro/detail/linux.hpp>

#include <chrono>
#include <mutex>

#include <liburing.h>

namespace cppcoro::detail::lnx
{
    class uring_queue;

    /// RAII IO transaction
    class [[nodiscard]] io_transaction final {
    public:
        io_transaction(uring_queue &queue, io_message& message) noexcept;
        bool commit() noexcept;

        [[nodiscard]] io_transaction &read(int fd, void *buffer, size_t size, size_t offset) noexcept;
        [[nodiscard]] io_transaction &write(int fd, const void * buffer, size_t size, size_t offset) noexcept;

        [[nodiscard]] io_transaction &readv(int fd, iovec* vec, size_t count, size_t offset) noexcept;
        [[nodiscard]] io_transaction &writev(int fd, iovec* vec, size_t count, size_t offset) noexcept;

        [[nodiscard]] io_transaction &recv(int fd, void * buffer, size_t size, int flags = 0) noexcept;
        [[nodiscard]] io_transaction &send(int fd, const void *buffer, size_t size, int flags = 0) noexcept;

        [[nodiscard]] io_transaction &recvmsg(int fd, msghdr *msg, int flags = 0) noexcept;
        [[nodiscard]] io_transaction &sendmsg(int fd, msghdr *msg, int flags = 0) noexcept;

        [[nodiscard]] io_transaction &connect(int fd, const void* to, size_t to_size) noexcept;
        [[nodiscard]] io_transaction &close(int fd) noexcept;

        [[nodiscard]] io_transaction &accept(int fd, const void* to, socklen_t* to_size, int flags = 0) noexcept;

        [[nodiscard]] io_transaction &timeout(__kernel_timespec *ts, bool absolute = false) noexcept;
        [[nodiscard]] io_transaction &timeout_remove(int flags = 0) noexcept;

        [[nodiscard]] io_transaction &nop() noexcept;

        [[nodiscard]] io_transaction &cancel(int flags = 0) noexcept;

    private:
        uring_queue &m_queue;
        io_message& m_message;
        std::scoped_lock<std::mutex> m_sqeLock;
        io_uring_sqe *m_sqe;
    };

	class uring_queue
	{
	public:
		explicit uring_queue(size_t queue_length = 32, uint32_t flags = 0);
		~uring_queue() noexcept;
		uring_queue(uring_queue&&) = delete;
		uring_queue& operator=(uring_queue&&) = delete;
		uring_queue(uring_queue const&) = delete;
		uring_queue& operator=(uring_queue const&) = delete;

		io_transaction transaction(io_message &message) noexcept;

		bool dequeue(io_message*& message, bool wait);

    private:
        friend class io_transaction;

		io_uring_sqe* get_sqe() noexcept;
        int submit() noexcept;

		std::mutex m_sqeMux;
		std::mutex m_outMux;
		io_uring ring_{};
	};
	using io_queue = uring_queue;
}  // namespace cppcoro::detail::lnx

#endif // CPPCORO_DETAIL_LINUX_URING_QUEUE_HPP_INCLUDED
