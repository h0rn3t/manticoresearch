//
// Copyright (c) 2017-2026, Manticore Software LTD (https://manticoresearch.com)
// Copyright (c) 2001-2016, Andrew Aksyonoff
// Copyright (c) 2008-2016, Sphinx Technologies Inc
// All rights reserved
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License. You should have
// received a copy of the GPL license along with this program; if you
// did not, you can find it at http://www.gnu.org/
//

#include "netpoll.h"

#include "std/timers.h"
#include "searchdaemon.h"
#include <memory>


class TimeoutEvents_c
{
	TimeoutQueue_c	m_dTimeouts;

public:
	constexpr static int64_t TIME_INFINITE = -1;
	constexpr static int64_t TIME_IMMEDIATE = 0;

	void AddOrChangeTimeout ( EnqueuedTimeout_t * pEvent )
	{
		if ( pEvent->m_iTimeoutTimeUS>=0 )
			m_dTimeouts.Change ( pEvent );
	}

	void RemoveTimeout ( EnqueuedTimeout_t * pEvent )
	{
		m_dTimeouts.Remove ( pEvent );
	}

	int64_t GetNextTimeoutUS ( int64_t iGranularity )
	{
		auto iDefaultTimeoutUS = TIME_INFINITE;
		while ( !m_dTimeouts.IsEmpty() )
		{
			auto * pNetEvent = (EnqueuedTimeout_t *) m_dTimeouts.Root ();
			assert ( pNetEvent->m_iTimeoutTimeUS>0 );

			auto iNextTimeoutUS = pNetEvent->m_iTimeoutTimeUS - MonoMicroTimer();
			if ( iNextTimeoutUS > iGranularity )
				return iNextTimeoutUS;
			else
				iDefaultTimeoutUS = TIME_IMMEDIATE;

			m_dTimeouts.Pop ();
		}
		return iDefaultTimeoutUS;
	}
};

// epoll keeps events in a hash, so we also track them in an explicit linked list;
// the list item pointer is stored in the backend's pPtr.

using NetPollEventsList_t = boost::intrusive::slist<NetPollEvent_t,
	boost::intrusive::member_hook<NetPollEvent_t, netlist_hook_t, &NetPollEvent_t::m_tBackHook>,
	boost::intrusive::constant_time_size<true>,
	boost::intrusive::cache_last<true>>;

// epoll poll traits; kept elementary and tiny for easy debugging


struct PollTraits_t
{
	using pollev = epoll_event;
	constexpr static int64_t poll_granularity = 1000LL;

	inline static int create_poller ( int iSizeHint ) { return epoll_create ( iSizeHint ); }
	inline static void close_poller ( int iPl )
	{
		if ( iPl >= 0 )
			sphSockClose ( iPl );
	}
	inline static void* get_data ( const pollev& tEv ) { return tEv.data.ptr; }
	inline static void translate_events ( const pollev& tEv )
	{
		auto* pNode = (NetPollEvent_t*)get_data ( tEv );
		assert ( pNode && "deleted event recognized" );
		if ( !pNode )
			return;

		pNode->m_uGotEvents = ( ( tEv.events & EPOLLERR ) ? NetPollEvent_t::IS_ERR : 0 )
							| ( ( tEv.events & EPOLLHUP ) ? NetPollEvent_t::IS_HUP : 0 )
							| ( ( tEv.events & EPOLLIN ) ? NetPollEvent_t::IS_READ : 0 )
							| ( ( tEv.events & EPOLLOUT ) ? NetPollEvent_t::IS_WRITE : 0 );
	}

	inline static int US2Polltime ( int64_t timeoutUS )
	{
		switch ( timeoutUS )
		{
		case TimeoutEvents_c::TIME_INFINITE: return -1;
		case TimeoutEvents_c::TIME_IMMEDIATE: return 0;
		default: return timeoutUS / poll_granularity;
		}
	}

	inline static int poll_events ( int iPoll, pollev* pEvents, int iEventNum, int64_t timeoutUS )
	{
		return epoll_wait ( iPoll, pEvents, iEventNum, US2Polltime ( timeoutUS ) );
	};

	inline static const char* epoll_action_name ( int iOp )
	{
		switch (iOp) { case EPOLL_CTL_ADD:return "EPOLL_CTL_ADD"; case EPOLL_CTL_MOD:return "EPOLL_CTL_MOD"; case EPOLL_CTL_DEL:return "EPOLL_CTL_DEL"; default:return "UNKNWON";};
	}

	inline static int set_polling_for ( int iPoll, int iSock, void* pData, BYTE, BYTE uIOChange, bool bAdd )
	{
		bool bRW = uIOChange & NetPollEvent_t::SET_RW;
		if ( !bRW && !bAdd )
			return 0;

		int iOp = bRW ? ( bAdd ? EPOLL_CTL_ADD : EPOLL_CTL_MOD ) : EPOLL_CTL_DEL;

		epoll_event tEv;
		if ( bRW )
		{
			tEv.data.ptr = pData;
			tEv.events = ( ( uIOChange & NetPollEvent_t::SET_ON_EDGE ) ? EPOLLET : 0 )
					   | ( ( uIOChange & NetPollEvent_t::SET_ONESHOT ) ? EPOLLONESHOT : 0 )
					   | ( ( uIOChange & NetPollEvent_t::SET_READ ) ? EPOLLIN : 0 )
					   | ( ( uIOChange & NetPollEvent_t::SET_WRITE ) ? EPOLLOUT : 0 );
			sphLogDebugv ( "%p epoll %d setup, ev=0x%u, op=%s, sock=%d", pData, iPoll, tEv.events, epoll_action_name ( iOp ), iSock );
		} else
			sphLogDebugv ( "%p epoll %d setup, op=%s, sock=%d", pData, iPoll, epoll_action_name ( iOp ), iSock );


		return epoll_ctl ( iPoll, iOp, iSock, &tEv );
	}

	inline static int polling_size ( int iQueueSize, int iMaxReady )
	{
		return iMaxReady ? Min ( iMaxReady, iQueueSize ) : iQueueSize;
	}
};


// need for remove from intrusive list to work
inline bool operator== ( const NetPollEvent_t& lhs, const NetPollEvent_t& rhs )
{
		return &lhs == &rhs;
}

// epoll backend implementation
class NetPooller_c::Impl_c final : public PollTraits_t
{
	friend class NetPooller_c;
	friend class NetPollReadyIterator_c;

	TimeoutEvents_c				m_dTimeouts			GUARDED_BY ( NetPoollingThread );
	CSphVector<pollev>			m_dFiredEvents		GUARDED_BY ( NetPoollingThread );
	NetPollEventsList_t			m_tEvents			GUARDED_BY ( NetPoollingThread ); // used for 'for_all'

	int							m_iReady = 0;
	const int 					m_iMaxReady;
	int							m_iLastReportedErrno = -1;
	int							m_iPl;

public:

	explicit Impl_c ( int iSizeHint, int iMaxReady )
		: m_iMaxReady (iMaxReady)
	{
		m_iPl = create_poller ( iSizeHint );
		if ( m_iPl==-1 )
			sphDie ( "failed to create poller main FD, errno=%d, %s", errno, strerrorm ( errno ) );

		sphLogDebugv ( "poller %d created", m_iPl );
		m_dFiredEvents.Reserve ( iSizeHint );
	}

	~Impl_c ()
	{
		sphLogDebugv ( "poller %d closed", m_iPl );
		close_poller ( m_iPl );
	}

	// called from working netloop routine
	void SetupEvent ( NetPollEvent_t * pEvent ) REQUIRES ( NetPoollingThread )
	{
		if ( pEvent->m_uIOChange == NetPollEvent_t::SET_CLOSED || pEvent->m_uIOChange == NetPollEvent_t::SET_NONE )
			return RemoveEvent ( pEvent );

		assert ( pEvent && pEvent->m_iSock>=0 );
		assert ( pEvent->m_uIOChange & NetPollEvent_t::SET_RW );

		m_dTimeouts.AddOrChangeTimeout ( pEvent );

		bool bIsNew = !pEvent->m_tBackHook.is_linked();
		if ( bIsNew )
		{
			SafeAddRef ( pEvent );
			m_tEvents.push_back ( *pEvent );
		}

		int iRes = set_polling_for ( m_iPl, pEvent->m_iSock, pEvent, pEvent->m_uIOActive, pEvent->m_uIOChange, bIsNew );
		pEvent->m_uIOActive = pEvent->m_uIOChange;

		if ( iRes == -1 )
			sphWarning ( "failed to setup queue event for sock %d, errno=%d, %s", pEvent->m_iSock, errno, strerrorm ( errno ) );
	}

	// called when client detected error or timeout, and even when netloop routine is not active (i.e., where it is stopped)
	void RemoveEvent ( NetPollEvent_t* pEvent ) REQUIRES ( NetPoollingThread )
	{
		assert ( pEvent );
		RemoveTimeout ( pEvent );

		sphLogDebugv ( "%p polling remove, ev=%u, sock=%d", pEvent, pEvent->m_uIOChange, pEvent->m_iSock );

		if ( pEvent->m_uIOChange != NetPollEvent_t::SET_CLOSED )
		{
			pEvent->m_uIOChange = NetPollEvent_t::SET_NONE;
			int iRes = set_polling_for ( m_iPl, pEvent->m_iSock, pEvent, pEvent->m_uIOActive, 0, true );

			// might be already closed by worker from thread pool
			if ( iRes == -1 )
				sphLogDebugv ( "failed to remove polling event for sock %d(%p), errno=%d, %s", pEvent->m_iSock, pEvent, errno, strerrorm ( errno ) );
		}

		// since event already removed from epoll - it is safe to remove it from the list of events also,
		// and totally unlink
		if ( pEvent->IsLinked() )
		{
			m_tEvents.remove ( *pEvent );
			SafeRelease ( pEvent );
		}
	}

	void Wait ( int64_t iUS ) REQUIRES ( NetPoollingThread )
	{
		if ( m_tEvents.empty() )
			return;

		if ( iUS==WAIT_UNTIL_TIMEOUT )
			iUS = m_dTimeouts.GetNextTimeoutUS ( poll_granularity );

		m_dFiredEvents.Resize ( polling_size ( m_tEvents.size(), m_iMaxReady ) );

		// need positive timeout for communicate threads back and shutdown
		m_iReady = poll_events ( m_iPl, m_dFiredEvents.Begin (), m_dFiredEvents.GetLength (), iUS );

		if ( m_iReady>=0 )
			return;

		int iErrno = sphSockGetErrno ();
		// common recoverable errors
		if ( iErrno==EINTR || iErrno==EAGAIN || iErrno==EWOULDBLOCK )
			return;

		if ( m_iLastReportedErrno!=iErrno )
		{
			sphWarning ( "polling tick failed: %s", sphSockError ( iErrno ) );
			m_iLastReportedErrno = iErrno;
		}
	}

	void ProcessAll ( std::function<void ( NetPollEvent_t * )>&& fnAction ) REQUIRES ( NetPoollingThread )
	{
		// not ranged-for here, as postfix ++ action for iterator is important (as fnAction can remove elem from list)
		for ( auto it { m_tEvents.begin() }, itend { m_tEvents.end() }; it != itend; )
			fnAction ( &*it++ );
	}

	int GetNumOfReady () const
	{
		return m_iReady;
	}

	void RemoveTimeout ( NetPollEvent_t * pEvent ) REQUIRES ( NetPoollingThread )
	{
		assert ( pEvent );
		m_dTimeouts.RemoveTimeout ( pEvent );
	}
};

// iterator over fired epoll events
NetPollEvent_t & NetPollReadyIterator_c::operator* ()
{
	auto & pOwner = m_pOwner->m_pImpl;
	const auto & tEv = pOwner->m_dFiredEvents[m_iIterEv];
	PollTraits_t::translate_events ( tEv );
	return *(NetPollEvent_t*)PollTraits_t::get_data ( tEv );
};

NetPollReadyIterator_c & NetPollReadyIterator_c::operator++ ()
{
	++m_iIterEv;
	return *this;
}

bool NetPollReadyIterator_c::operator!= ( const NetPollReadyIterator_c & rhs ) const
{
	auto & pOwner = m_pOwner->m_pImpl;
	return rhs.m_pOwner || m_iIterEv<pOwner->m_iReady;
}



NetPooller_c::NetPooller_c ( int isizeHint, int iMaxReady )
	: m_pImpl ( std::make_unique<Impl_c> ( isizeHint, iMaxReady ) )
{}

NetPooller_c::~NetPooller_c () = default;

void NetPooller_c::SetupEvent ( NetPollEvent_t * pEvent )
{
	m_pImpl->SetupEvent ( pEvent );
}

void NetPooller_c::Wait ( int64_t iUS )
{
	m_pImpl->Wait ( iUS );
}

int NetPooller_c::GetNumOfReady () const
{
	return m_pImpl->GetNumOfReady();
}

void NetPooller_c::ProcessAll ( std::function<void ( NetPollEvent_t * )>&& fnAction )
{
	m_pImpl->ProcessAll ( std::move ( fnAction ) );
}

void NetPooller_c::RemoveTimeout ( NetPollEvent_t * pEvent )
{
	m_pImpl->RemoveTimeout ( pEvent );
}

void NetPooller_c::RemoveEvent ( NetPollEvent_t * pEvent )
{
	m_pImpl->RemoveEvent ( pEvent );
}

int64_t NetPooller_c::TickGranularity() const
{
	return m_pImpl->poll_granularity;
}


ThreadRole NetPoollingThread;
