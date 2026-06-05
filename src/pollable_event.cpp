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

#include "pollable_event.h"

#include "searchdaemon.h"

#include "config.h"
#if HAVE_EVENTFD
#include <sys/eventfd.h>
#endif

#if !HAVE_EVENTFD
static bool CreateSocketPair ( int &iSock1, int &iSock2, CSphString &sError )
{
	int dSockets[2] = { -1, -1 };
	if ( socketpair ( AF_LOCAL, SOCK_STREAM, 0, dSockets )!=0 )
	{
		sError.SetSprintf ( "failed to create socketpair: %s", sphSockError () );
		return false;
	}

	iSock1 = dSockets[0];
	iSock2 = dSockets[1];


	if ( sphSetSockNB ( iSock1 )<0 || sphSetSockNB ( iSock2 )<0 )
	{
		sError.SetSprintf ( "failed to set socket non-block: %s", sphSockError () );
		SafeCloseSocket ( iSock1 );
		SafeCloseSocket ( iSock2 );
		return false;
	}
	return true;
}
#endif


PollableEvent_t::PollableEvent_t ()
{
	int iRead = -1;
	int iWrite = -1;
#if HAVE_EVENTFD
	int iFD = eventfd ( 0, EFD_NONBLOCK );
	if ( iFD==-1 )
		m_sError.SetSprintf ( "failed to create eventfd: %s", strerrorm ( errno ) );
	iRead = iWrite = iFD;
#else
	CreateSocketPair ( iRead, iWrite, m_sError );
#endif

	if ( iRead==-1 || iWrite==-1 )
		sphWarning ( "PollableEvent_t create error:%s", m_sError.cstr () );
	m_iPollablefd = iRead;
	m_iSignalEvent = iWrite;
}

PollableEvent_t::~PollableEvent_t ()
{
	Close ();
}

void PollableEvent_t::Close ()
{
	SafeCloseSocket ( m_iPollablefd );
#if !HAVE_EVENTFD
	SafeCloseSocket ( m_iSignalEvent );
#endif
}

int PollableEvent_t::PollableErrno ()
{
	return sphSockGetErrno ();
}

bool PollableEvent_t::FireEvent () const
{
	if ( m_iSignalEvent==-1 )
		return true;

	int iErrno = EAGAIN;
	while ( iErrno==EAGAIN || iErrno==EWOULDBLOCK )
	{
		uint64_t uVal = 1;
#if HAVE_EVENTFD
		int iPut = ::write ( m_iSignalEvent, &uVal, 8 );
#else
		int iPut = sphSockSend ( m_iSignalEvent, ( const char * ) &uVal, 8 );
#endif
		if ( iPut==8 )
			return true;
		iErrno = PollableErrno ();
	};
	return false;
}

// just wipe-out a fired event to free queue, we don't need the value itself
void PollableEvent_t::DisposeEvent () const
{
	assert ( m_iPollablefd!=-1 );
	uint64_t uVal = 0;
	while ( true )
	{
#if HAVE_EVENTFD
		auto iRead = ::read ( m_iPollablefd, &uVal, 8 );
		if ( iRead==8 )
			break;
#else
		// socket-pair case might stack up some values and these should be read
		int iRead = sphSockRecv ( m_iPollablefd, ( char * ) &uVal, 8 );
		if ( iRead<=0 )
			break;
#endif
	}
}
