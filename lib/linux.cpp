///////////////////////////////////////////////////////////////////////////////
// Copyright (c) Microsoft
// Licenced under MIT license. See LICENSE.txt for details.
///////////////////////////////////////////////////////////////////////////////

#include <cppcoro/detail/linux.hpp>
#include <unistd.h>

namespace cppcoro::detail::lnx
{
	void safe_fd::close() noexcept
	{
		if (m_fd != -1)
		{
			::close(m_fd);
			m_fd = -1;
		}
	}
}  // namespace cppcoro::detail::lnx
