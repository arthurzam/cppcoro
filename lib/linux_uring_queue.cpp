#include <cppcoro/detail/linux_uring_queue.hpp>

namespace cppcoro::detail::lnx {

    io_transaction::io_transaction(io_queue &queue, io_message &message) noexcept
        : m_queue{queue}, m_message{message}, m_sqeLock{queue.m_sqeMux}, m_sqe{queue.get_sqe()} {
    }

    [[nodiscard]] bool io_transaction::commit() noexcept {
        if (m_sqe != nullptr) {
            if (int err = m_queue.submit(); err < 0) {
                m_message.result = err;
                return false;
            }
            return true;
        } else {
            // no stream resource
            m_message.result = -ENOSR;
            return false;
        }
    }

    io_transaction &io_transaction::read(int fd, void *buffer, size_t size, size_t offset) noexcept {
        if (m_sqe) {
            io_uring_prep_read(m_sqe, fd, buffer, size, offset);
            io_uring_sqe_set_data(m_sqe, &m_message);
        }
        return *this;
    }

    io_transaction &io_transaction::write(int fd, const void *buffer, size_t size, size_t offset) noexcept {
        if (m_sqe) {
            io_uring_prep_write(m_sqe, fd, buffer, size, offset);
            io_uring_sqe_set_data(m_sqe, &m_message);
        }
        return *this;
    }

    io_transaction &io_transaction::recv(int fd, void *buffer, size_t size, int flags) noexcept {
        if (m_sqe) {
            io_uring_prep_recv(m_sqe, fd, buffer, size, flags);
            io_uring_sqe_set_data(m_sqe, &m_message);
        }
        return *this;
    }

    io_transaction &io_transaction::send(int fd, const void *buffer, size_t size, int flags) noexcept {
        if (m_sqe) {
            io_uring_prep_send(m_sqe, fd, buffer, size, flags);
            io_uring_sqe_set_data(m_sqe, &m_message);
        }
        return *this;
    }

    io_transaction &io_transaction::readv(int fd, iovec *vec, size_t count, size_t offset) noexcept {
        if (m_sqe) {
            io_uring_prep_readv(m_sqe, fd, vec, count, offset);
            io_uring_sqe_set_data(m_sqe, &m_message);
        }
        return *this;
    }

    io_transaction &io_transaction::writev(int fd, iovec *vec, size_t count, size_t offset) noexcept {
        if (m_sqe) {
            io_uring_prep_writev(m_sqe, fd, vec, count, offset);
            io_uring_sqe_set_data(m_sqe, &m_message);
        }
        return *this;
    }

    io_transaction &io_transaction::recvmsg(int fd, msghdr *msg, int flags) noexcept {
        if (m_sqe) {
            io_uring_prep_recvmsg(m_sqe, fd, msg, flags);
            io_uring_sqe_set_data(m_sqe, &m_message);
        }
        return *this;
    }

    io_transaction &io_transaction::sendmsg(int fd, msghdr *msg, int flags) noexcept {
        if (m_sqe) {
            io_uring_prep_sendmsg(m_sqe, fd, msg, flags);
            io_uring_sqe_set_data(m_sqe, &m_message);
        }
        return *this;
    }

    io_transaction &io_transaction::connect(int fd, const void *to, size_t to_size) noexcept {
        if (m_sqe) {
            io_uring_prep_connect(m_sqe, fd, reinterpret_cast<sockaddr*>(const_cast<void*>(to)), to_size);
            io_uring_sqe_set_data(m_sqe, &m_message);
        }
        return *this;
    }

    io_transaction &io_transaction::close(int fd) noexcept {
        if (m_sqe) {
            io_uring_prep_close(m_sqe, fd);
            io_uring_sqe_set_data(m_sqe, &m_message);
        }
        return *this;
    }

    io_transaction &io_transaction::accept(int fd, const void *to, socklen_t *to_size, int flags) noexcept {
        if (m_sqe) {
            io_uring_prep_accept(m_sqe, fd, reinterpret_cast<sockaddr*>(const_cast<void*>(to)), to_size, flags);
            io_uring_sqe_set_data(m_sqe, &m_message);
        }
        return *this;
    }

    io_transaction &io_transaction::timeout(__kernel_timespec *ts, bool absolute) noexcept {
        if (m_sqe) {
            io_uring_prep_timeout(m_sqe, ts, 0, absolute ? IORING_TIMEOUT_ABS : 0);
            io_uring_sqe_set_data(m_sqe, &m_message);
        }
        return *this;
    }

    io_transaction &io_transaction::timeout_remove(int flags) noexcept {
        if (m_sqe) {
            io_uring_prep_timeout_remove(m_sqe, reinterpret_cast<uint64_t>(&m_message), flags);
        }
        return *this;
    }

    io_transaction &io_transaction::nop() noexcept {
        if (m_sqe) {
            io_uring_prep_nop(m_sqe);
            io_uring_sqe_set_data(m_sqe, &m_message);
        }
        return *this;
    }

    io_transaction &io_transaction::cancel(int flags) noexcept {
        if (m_sqe) {
            io_uring_prep_cancel(m_sqe, &m_message, flags);
            m_message.result = -ECANCELED;
        }
        return *this;
    }

    uring_queue::uring_queue(size_t queue_length, uint32_t flags) {
        auto err = io_uring_queue_init(queue_length, &ring_, flags);
        if (err < 0) {
            throw std::system_error{static_cast<int>(-err),
                                    std::system_category(),
                                    "Error initializing uring"};
        }
    }

    uring_queue::~uring_queue() noexcept { io_uring_queue_exit(&ring_); }

    io_transaction uring_queue::transaction(io_message &message) noexcept {
        return io_transaction(*this, message);
    }

    io_uring_sqe *uring_queue::get_sqe() noexcept {
        return io_uring_get_sqe(&ring_);
    }

    int uring_queue::submit() noexcept {
        int res = io_uring_submit(&ring_);
        return res;
    }

    bool uring_queue::dequeue(detail::lnx::io_message *&msg, bool wait) {
        std::lock_guard guard(m_outMux);
        io_uring_cqe *cqe;
        int ret;
        if (wait)
            ret = io_uring_wait_cqe(&ring_, &cqe);
        else
            ret = io_uring_peek_cqe(&ring_, &cqe);
        if (ret == -EAGAIN) {
            return false;
        } else if (ret < 0) {
            throw std::system_error{-ret,
                                    std::system_category(),
                                    std::string{"io_uring_peek_cqe failed"}};
        } else {
            msg = reinterpret_cast<detail::lnx::io_message *>(io_uring_cqe_get_data(cqe));
            if (msg != nullptr
                && msg->result == -1) // manually set result eg.: -ECANCEL
            {
                msg->result = cqe->res;
            }
            io_uring_cqe_seen(&ring_, cqe);
            return true;  // completed
        }
    }

}  // namespace cppcoro::detail::lnx
