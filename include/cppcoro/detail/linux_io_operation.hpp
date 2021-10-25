///////////////////////////////////////////////////////////////////////////////
// Copyright (c) Lewis Baker
// Licenced under MIT license. See LICENSE.txt for details.
///////////////////////////////////////////////////////////////////////////////
#ifndef CPPCORO_DETAIL_LINUX_IO_OPERATION_HPP_INCLUDED
#define CPPCORO_DETAIL_LINUX_IO_OPERATION_HPP_INCLUDED

#include <cppcoro/io_service.hpp>
#include <cppcoro/detail/linux.hpp>

#include <cppcoro/coroutine.hpp>
#include <arpa/inet.h>
#include <cassert>
#include <cstring>
#include <optional>
#include <system_error>

#if CPPCORO_USE_IO_RING
#include <cppcoro/detail/linux_uring_queue.hpp>
#else
#include <cppcoro/detail/linux_epoll_queue.hpp>
#endif

#include <cppcoro/operation_cancelled.hpp>

#include <sys/timerfd.h>

namespace cppcoro::detail
{
#if CPPCORO_USE_IO_RING
    using event_timespec = __kernel_timespec;
#else
    using event_timespec = timespec;
#endif

    template<typename _Rep, typename _Period>
    constexpr event_timespec duration_to_event_timespec(std::chrono::duration<_Rep, _Period> dur)
    {
        using namespace std::chrono;

        auto ns = duration_cast<nanoseconds>(dur);
        auto secs = duration_cast<seconds>(dur);
        ns -= secs;

        return {secs.count(), ns.count()};
    }

    template<typename _Clock, typename _Dur>
    constexpr event_timespec time_point_to_event_timespec(std::chrono::time_point<_Clock, _Dur> tp)
    {
        using namespace std::chrono;

        auto secs = time_point_cast<seconds>(tp);
        auto ns = time_point_cast<nanoseconds>(tp) -
                  time_point_cast<nanoseconds>(secs);

        return {secs.time_since_epoch().count(), ns.count()};
    }

	class io_operation_base
	{
	public:
        io_operation_base(lnx::io_queue& ioService, size_t offset = 0) noexcept
			: m_ioQueue(ioService)
			, m_offset(offset)
			, m_message{}
		{
		}

		std::size_t get_result()
		{
			if (m_message.result < 0)
			{
				throw std::system_error{ -m_message.result, std::system_category() };
			}

			return m_message.result;
		}

		size_t m_offset;
        detail::lnx::io_message m_message;
		detail::lnx::io_queue& m_ioQueue;
	};

	template<typename OPERATION>
	class io_operation : protected io_operation_base
	{
	protected:
        io_operation(lnx::io_queue& ioService, size_t offset = 0) noexcept
			: io_operation_base(ioService, offset)
		{
		}

	public:
		bool await_ready() const noexcept { return false; }

		CPPCORO_NOINLINE
		bool await_suspend(coroutine_handle<> awaitingCoroutine)
		{
			static_assert(std::is_base_of_v<io_operation, OPERATION>);

			m_message = awaitingCoroutine;
			return static_cast<OPERATION*>(this)->try_start();
		}

		decltype(auto) await_resume() {
			return static_cast<OPERATION*>(this)->get_result();
		}
	};

    template<typename OPERATION>
    class io_operation_cancellable : protected io_operation<OPERATION>
    {
    protected:
        // ERROR_OPERATION_ABORTED value from <errno.h>
        static constexpr int error_operation_aborted = -ECANCELED;

        io_operation_cancellable(lnx::io_queue& ioQueue, cancellation_token&& ct) noexcept
            : io_operation<OPERATION>(ioQueue, 0)
            , m_state(ct.is_cancellation_requested() ? state::completed : state::not_started)
            , m_cancellationToken(std::move(ct))
        {
        }

        io_operation_cancellable(
            lnx::io_queue& ioQueue, size_t offset, cancellation_token&& ct) noexcept
            : io_operation<OPERATION>(ioQueue, offset)
            , m_state(ct.is_cancellation_requested() ? state::completed : state::not_started)
            , m_cancellationToken(std::move(ct))
        {
        }

    public:
        bool await_ready() const noexcept
        {
            return m_state.load(std::memory_order_relaxed) == state::completed;
        }

        CPPCORO_NOINLINE
        bool await_suspend(coroutine_handle<> awaitingCoroutine) noexcept
        {
            static_assert(std::is_base_of_v<io_operation_cancellable, OPERATION>);

            this->m_message = awaitingCoroutine;

            // TRICKY: Register cancellation callback before starting the operation
            // in case the callback registration throws due to insufficient
            // memory. We need to make sure that the logic that occurs after
            // starting the operation is noexcept, otherwise we run into the
            // problem of not being able to cancel the started operation and
            // the dilemma of what to do with the exception.
            //
            // However, doing this means that the cancellation callback may run
            // prior to returning below so in the case that cancellation may
            // occur we defer setting the state to 'started' until after
            // the operation has finished starting. The cancellation callback
            // will only attempt to request cancellation of the operation with
            // CancelIoEx() once the state has been set to 'started'.
            if (m_cancellationToken.is_cancellation_requested())
            {
                this->m_message.result = error_operation_aborted;
                return false;
            }

            const bool canBeCancelled = m_cancellationToken.can_be_cancelled();
            m_state.store(state::started, std::memory_order_relaxed);

            // Now start the operation.
            const bool willCompleteAsynchronously = static_cast<OPERATION*>(this)->try_start();
            if (!willCompleteAsynchronously)
            {
                // Operation completed synchronously, resume awaiting coroutine immediately.
                this->m_message.result = error_operation_aborted;
                return false;
            }

            if (canBeCancelled)
            {
                // Need to flag that the operation has finished starting now.

                // However, the operation may have completed concurrently on
                // another thread, transitioning directly from not_started -> complete.
                // Or it may have had the cancellation callback execute and transition
                // from not_started -> cancellation_requested. We use a compare-exchange
                // to determine a winner between these potential racing cases.
                state oldState = state::not_started;
                if (!m_state.compare_exchange_strong(
                    oldState,
                    state::started,
                    std::memory_order_release,
                    std::memory_order_acquire))
                {
                    if (oldState == state::cancellation_requested)
                    {
                        // Request the operation be cancelled.
                        // Note that it may have already completed on a background
                        // thread by now so this request for cancellation may end up
                        // being ignored.
                        static_cast<OPERATION*>(this)->cancel();

                        if (!m_state.compare_exchange_strong(
                            oldState,
                            state::started,
                            std::memory_order_release,
                            std::memory_order_acquire))
                        {
                            assert(oldState == state::completed);
                            this->m_message.result = error_operation_aborted;
                            return false;
                        }
                    }
                    else
                    {
                        m_cancellationRegistration.emplace(std::move(m_cancellationToken), [this] {
                            m_state.store(state::cancellation_requested, std::memory_order_seq_cst);
                            static_cast<OPERATION*>(this)->cancel();
                        });
                        assert(oldState == state::started);
                        return true;
                    }
                }
            }

            return true;
        }

        decltype(auto) await_resume()
        {
            if (this->m_message.result == error_operation_aborted)
            {
                throw operation_cancelled{};
            }
            else if (this->m_message.result < 0)
            {
                if (this->m_message.result == -EINTR &&
                    m_state.load(std::memory_order_acquire) == state::cancellation_requested)
                {
                    throw operation_cancelled{};
                }
                throw std::system_error{ -this->m_message.result, std::system_category() };
            }

            return static_cast<OPERATION*>(this)->get_result();
        }

    private:
        enum class state
        {
            not_started,
            started,
            cancellation_requested,
            completed
        };

        void on_cancellation_requested() noexcept
        {
            auto oldState = m_state.load(std::memory_order_acquire);
            if (oldState == state::not_started)
            {
                // This callback is running concurrently with await_suspend().
                // The call to start the operation may not have returned yet so
                // we can't safely request cancellation of it. Instead we try to
                // notify the await_suspend() thread by transitioning the state
                // to state::cancellation_requested so that the await_suspend()
                // thread can request cancellation after it has finished starting
                // the operation.
                const bool transferredCancelResponsibility = m_state.compare_exchange_strong(
                    oldState,
                    state::cancellation_requested,
                    std::memory_order_release,
                    std::memory_order_acquire);
                if (transferredCancelResponsibility)
                {
                    return;
                }
            }

            // No point requesting cancellation if the operation has already completed.
            if (oldState != state::completed)
            {
                static_cast<OPERATION*>(this)->cancel();
            }
        }

        std::atomic<state> m_state;
        cppcoro::cancellation_token m_cancellationToken;
        std::optional<cppcoro::cancellation_registration> m_cancellationRegistration;
    };

}  // namespace cppcoro::detail

#endif  // CPPCORO_DETAIL_LINUX_IO_OPERATION_HPP_INCLUDED
