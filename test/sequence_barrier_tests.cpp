///////////////////////////////////////////////////////////////////////////////
// Copyright (c) Lewis Baker
// Licenced under MIT license. See LICENSE.txt for details.
///////////////////////////////////////////////////////////////////////////////

#include <cppcoro/sequence_barrier.hpp>

#include <cppcoro/config.hpp>
#include <cppcoro/task.hpp>
#include <cppcoro/sync_wait.hpp>
#include <cppcoro/when_all.hpp>

#if CPPCORO_OS_WINNT
# include <cppcoro/io_service.hpp>
# include <cppcoro/on_scope_exit.hpp>

#include <stdio.h>
# include <thread>
#endif

#include "doctest/doctest.h"

DOCTEST_TEST_SUITE_BEGIN("sequence_barrier");

using namespace cppcoro;

DOCTEST_TEST_CASE("default construction")
{
	sequence_barrier<std::uint32_t> barrier;
	CHECK(barrier.last_published() == sequence_traits<std::uint32_t>::initial_sequence);
	barrier.publish(3);
	CHECK(barrier.last_published() == 3);
}

DOCTEST_TEST_CASE("constructing with initial sequence number")
{
	sequence_barrier<std::uint64_t> barrier{ 100 };
	CHECK(barrier.last_published() == 100);
}

DOCTEST_TEST_CASE("wait_until_published single-threaded")
{
	sequence_barrier<std::uint32_t> barrier;
	bool reachedA = false;
	bool reachedB = false;
	bool reachedC = false;
	bool reachedD = false;
	bool reachedE = false;
	bool reachedF = false;
	sync_wait(when_all(
		[&]() -> task<>
		{
			CHECK(co_await barrier.wait_until_published(0) == 0);
			reachedA = true;
			CHECK(co_await barrier.wait_until_published(1) == 1);
			reachedB = true;
			CHECK(co_await barrier.wait_until_published(3) == 3);
			reachedC = true;
			CHECK(co_await barrier.wait_until_published(4) == 10);
			reachedD = true;
			co_await barrier.wait_until_published(5);
			reachedE = true;
			co_await barrier.wait_until_published(10);
			reachedF = true;
		}(),
		[&]() -> task<>
		{
			CHECK(!reachedA);
			barrier.publish(0);
			CHECK(reachedA);
			CHECK(!reachedB);
			barrier.publish(1);
			CHECK(reachedB);
			CHECK(!reachedC);
			barrier.publish(2);
			CHECK(!reachedC);
			barrier.publish(3);
			CHECK(reachedC);
			CHECK(!reachedD);
			barrier.publish(10);
			CHECK(reachedD);
			CHECK(reachedE);
			CHECK(reachedF);
			co_return;
		}()));
	CHECK(reachedF);
}

DOCTEST_TEST_CASE("wait_until_published multiple awaiters")
{
	sequence_barrier<std::uint32_t> barrier;
	bool reachedA = false;
	bool reachedB = false;
	bool reachedC = false;
	bool reachedD = false;
	bool reachedE = false;
	sync_wait(when_all(
		[&]() -> task<>
	{
		CHECK(co_await barrier.wait_until_published(0) == 0);
		reachedA = true;
		CHECK(co_await barrier.wait_until_published(1) == 1);
		reachedB = true;
		CHECK(co_await barrier.wait_until_published(3) == 3);
		reachedC = true;
	}(),
		[&]() -> task<>
	{
		CHECK(co_await barrier.wait_until_published(0) == 0);
		reachedD = true;
		CHECK(co_await barrier.wait_until_published(3) == 3);
		reachedE = true;
	}(),
		[&]() -> task<>
	{
		CHECK(!reachedA);
		CHECK(!reachedD);
		barrier.publish(0);
		CHECK(reachedA);
		CHECK(reachedD);
		CHECK(!reachedB);
		CHECK(!reachedE);
		barrier.publish(1);
		CHECK(reachedB);
		CHECK(!reachedC);
		CHECK(!reachedE);
		barrier.publish(2);
		CHECK(!reachedC);
		CHECK(!reachedE);
		barrier.publish(3);
		CHECK(reachedC);
		CHECK(reachedE);
		co_return;
	}()));
	CHECK(reachedC);
	CHECK(reachedE);
}

#if CPPCORO_OS_WINNT

DOCTEST_TEST_CASE("multi-threaded usage single consumer")
{
	io_service ioSvc;

	// Spin up 2 io threads
	std::thread ioThread1{ [&] { ioSvc.process_events(); } };
	auto joinOnExit1 = on_scope_exit([&] { ioThread1.join(); });
	auto stopOnExit1 = on_scope_exit([&] { ioSvc.stop(); });
	std::thread ioThread2{ [&] { ioSvc.process_events(); } };
	auto joinOnExit2 = on_scope_exit([&] { ioThread2.join(); });
	auto stopOnExit2 = std::move(stopOnExit1);

	sequence_barrier<std::size_t> writeBarrier;
	sequence_barrier<std::size_t> readBarrier;

	constexpr std::size_t iterationCount = 1'000'000;

	constexpr std::size_t bufferSize = 256;
	std::uint64_t buffer[bufferSize];

	auto[result, dummy] = sync_wait(when_all(
		[&]() -> task<std::uint64_t>
	{
		// Consumer
		co_await ioSvc.schedule();

		std::uint64_t sum = 0;

		bool reachedEnd = false;
		std::size_t nextToRead = 0;
		do
		{
			std::size_t available = writeBarrier.last_published();
			if (sequence_traits<std::size_t>::precedes(available, nextToRead))
			{
				available = co_await writeBarrier.wait_until_published(nextToRead);
				co_await ioSvc.schedule();
			}

			do
			{
				sum += buffer[nextToRead % bufferSize];
			} while (nextToRead++ != available);

			// Zero value is sentinel that indicates the end of the stream.
			reachedEnd = buffer[available % bufferSize] == 0;

			// Notify that we've finished processing up to 'available'.
			readBarrier.publish(available);
		} while (!reachedEnd);

		co_return sum;
	}(),
		[&]() -> task<>
	{
		// Producer
		co_await ioSvc.schedule();

		std::size_t available = readBarrier.last_published() + bufferSize;
		for (std::size_t nextToWrite = 0; nextToWrite <= iterationCount; ++nextToWrite)
		{
			if (sequence_traits<std::size_t>::precedes(available, nextToWrite))
			{
				available = co_await readBarrier.wait_until_published(nextToWrite - bufferSize) + bufferSize;
				co_await ioSvc.schedule();
			}

			if (nextToWrite == iterationCount)
			{
				// Write sentinel (zero) as last element.
				buffer[nextToWrite % bufferSize] = 0;
			}
			else
			{
				// Write value
				buffer[nextToWrite % bufferSize] = nextToWrite + 1;
			}

			// Notify consumer that we've published a new value.
			writeBarrier.publish(nextToWrite);
		}
	}()));

	// Suppress unused variable warning.
	(void)dummy;

	constexpr std::uint64_t expectedResult =
		std::uint64_t(iterationCount) * std::uint64_t(iterationCount + 1) / 2;

	CHECK(result == expectedResult);
}

#endif

DOCTEST_TEST_SUITE_END();