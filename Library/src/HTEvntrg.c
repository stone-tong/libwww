/*				    				     HTEvntrg.c
**	EVENT MANAGER
**
**	(c) COPYRIGHT MIT 1995.
**	Please first read the full copyright statement in the file COPYRIGH.
**
**	Updated HTEvent module 
**	This new module combines the functions of the old HTEvent module and 
**	the HTThread module. We retain the old HTThread module, but it
**	consists of calls to the HTEvent interfaces
**
** Authors:
**	HFN	Henrik Frystyk <frystyk@w3.org>
**	CLB    	Charlie Brooks <cbrooks@osf.org>
** Bugs
**
*/

#include <assert.h>			/* @@@ Should be in tcp.h @@@ */

/* Implementation dependent include files */
#include "tcp.h"
#include "HTUtils.h"
#include "HTString.h"
#include "HTError.h"
#include "HTReqMan.h"
#include "HTDNS.h"
#include "HTEvntrg.h"					 /* Implemented here */

/* Type definitions and global variables etc. local to this module */
PRIVATE fd_set read_fds ;		   /* sockets registered for reading */
PRIVATE fd_set write_fds;				   /* for Writing... */
PRIVATE fd_set except_fds;			       /* for exceptions ... */
PRIVATE fd_set all_fds ;			    /* any descriptor at all */

PRIVATE CONST int SecondsToWait = 5 ;

PRIVATE void __ResetMaxSock( void ) ;
PRIVATE int __DoCallBack( SOCKET, SockOps);
PRIVATE int __DoUserCallBack( SOCKET, SockOps);
PRIVATE void __DumpFDSet( fd_set *, CONST char *);

typedef unsigned long DWORD;

/*
** our internal structure to hold a socket, it's request 
** and our action. For version 1, we allow one action per socket
*/

typedef struct rq_t RQ;

/*
** an action consists of a request, a set of requested operations 
** a HTEventCallBack function, and a priority (priority is not used in this
** version)
*/

typedef struct action_t  { 
    HTRequest * rq ;    				/* request structure */
    SockOps ops ;         			     /* requested operations */
    HTEventCallBack *cbf;      		     /* callback function to execute */
    HTPriority p;         	     /* priority associated with this socket */
} ACTION ;

struct rq_t { 
    RQ * next ;	  
    SOCKET s ;		 	/* our socket */
    BOOL unregister;		/* notify app when completely unregistered */
    ACTION actions[1];
};

#define PRIME_TABLE_SIZE 67 
#define MAX_ACTIONS 1 

#define HASH(s) ((s) % PRIME_TABLE_SIZE) 

/*
** our internal table of requests,
** indexed by socket 
*/
PRIVATE RQ * table[PRIME_TABLE_SIZE]; 
PRIVATE SOCKET max_sock = 0 ;		/* max socket value in use */
PRIVATE int socketsInUse = 0 ;		/* actual sockets in use */
PRIVATE int userSockets = 0;		/* Number of user sockets */
PRIVATE BOOL console_in_use = NO;

#ifndef _WINDOWS 
typedef void * HANDLE ;
#endif

PRIVATE HANDLE console_handle = 0 ;

/* Select Timeout handling */
typedef struct _HTTimeout {
    HTEventTimeout *	tcbf;
    HTRequest *		request;
    struct timeval	tv;
    BOOL		always;
} HTTimeout;

PRIVATE HTTimeout 	seltime;
PRIVATE struct timeval *tvptr = NULL;

/*
** this set of SockOps map our WinSock "socket event SockOps" into 
** our read and write sets. Note that under the canonical Unix model,
** a non-blocking socket passed to an accept() call will appear as readable, 
** whilst a non-blocking call to connect() will appear as writeable. In add.
** if the connection has been closed, the socket will appear readable under
** BSD Unix semantics 
*/
PRIVATE CONST SockOps ReadBits = FD_READ | FD_ACCEPT  | FD_CLOSE;
PRIVATE CONST SockOps WriteBits = FD_WRITE | FD_CONNECT ;
PRIVATE CONST SockOps ExceptBits = FD_OOB ;

/*
** Local functions 
*/
PRIVATE int __HTEvent_addRequest( SOCKET, HTRequest *, SockOps, HTEventCallBack *, HTPriority); 
PRIVATE void __RequestInit( RQ *, SOCKET, HTRequest *, SockOps, HTEventCallBack *, HTPriority);
PRIVATE int __ProcessFds( fd_set *, SockOps, CONST char *);
PRIVATE void __RequestUpdate( RQ *, SOCKET, HTRequest *, SockOps, HTEventCallBack *, HTPriority);
PRIVATE int __EventUnregister(RQ * , RQ **, SockOps );

/* ------------------------------------------------------------------------- */

/*	HTEvent_registerTimeout 
**	-----------------------
**	Set the timeout for sockets. This does only works on NON windows
**	platforms as we need to poll for the console on windows
**	The default is no timeout. If the tp points to a zero'ed structure
**	then the select is basically polling. If always is YES then
**	the callback is called at all times, if NO then only when Library
**	sockets are active.
**	Returns YES if OK else NO
*/
PUBLIC BOOL HTEvent_registerTimeout (struct timeval *tp, HTRequest * request,
				     HTEventTimeout *tcbf, BOOL always)
{
#ifdef _WIN32
    return NO;
#else
    if (tp) {
	tvptr = &seltime.tv;
	tvptr->tv_sec = tp->tv_sec;
	tvptr->tv_usec = tp->tv_usec;
	seltime.tcbf = tcbf;
	seltime.request = request;
	seltime.always = always;
	if (THD_TRACE)
	    fprintf(TDEST,"Timeout cbf. %p %s (request=%p, sec=%d, usec=%d)\n",
		    tcbf, always ? "always" : "active",
		    request, (int) tp->tv_sec, (int) tp->tv_usec);
    }
    return YES;
#endif
}

/*	HTEvent_unregisterTimeout 
**	-------------------------
**	Disables the callback function.
**	Returns YES if OK else NO
*/
PUBLIC BOOL HTEvent_unregisterTimeout (void)
{
    tvptr = NULL;
    return YES;
}

/*
** HTEvent_RegisterTTY 
**
** register the tty (console) as having events.
** if the TTY is select()-able (as is true under Unix), then we treat
** it as just another socket. Otherwise, take steps depending on the platform
*/
PUBLIC int HTEvent_RegisterTTY( SOCKET fd, HTRequest * rq, SockOps ops, 
			       HTEventCallBack *cbf, HTPriority p) 
{
    assert(rq != 0);
    console_in_use = YES;
    console_handle = (HANDLE) fd;

    if (THD_TRACE) 
	fprintf(TDEST, 
		"RegisterTTY. socket %d, request %p HTEventCallBack %p SockOps %x at priority %d\n",
		fd, (void *)rq,  (void *)cbf, (unsigned) ops, (unsigned) p);
	
#ifdef TTY_IS_SELECTABLE 

    /* HTEvent_Register adds the request _and_ inserts in fd table */
    userSockets++;
    return HTEvent_Register( fd, rq, ops, cbf, p) ;

#else 

#ifdef _WINDOWS	/* should be true for Windows3.x and NT and 95 */

    if (rq->hwnd != 0)  /* Windows GUI processing requested */
    	return 0;
    if (THD_TRACE) 
   	fprintf(TDEST, "RegisterTTY. Windows, and no handle given\n");

    console_handle = GetStdHandle( STD_INPUT_HANDLE) ;
 
    return __HTEvent_addRequest((SOCKET)console_handle, rq, ops, cbf, p);
#else 
#error "Don't know how to handle console tty for this system!"
#endif /* WINDOWS */
#endif /* TTY_IS_SELECTABLE */
}

/*
** HTEvent_UnRegisterTTY - unregisters TTY i/o channel
*/
PUBLIC int HTEvent_UnRegisterTTY(SOCKET s, SockOps ops) 
{
    if (THD_TRACE)
	fprintf(TDEST, "UnregisterTTY on channel %d\n", s) ;
    if (!console_in_use)
    	return 0;
    console_in_use = NO;

#ifdef _WINDOWS
    s = (SOCKET)console_handle ;
#endif
#ifdef TTY_IS_SELECTABLE
    userSockets--;
#endif
    return HTEvent_UnRegister(s, ops) ;	/* no harm in unregistering...*/
}


/*
**  HTEvent_Register
**  for a given socket, reqister a request structure, a set of operations, 
**  a HTEventCallBack function, and a priority. For this implementation, 
**  we allow only a single HTEventCallBack function for all operations.
**  and the priority field is ignored.
*/
PUBLIC int HTEvent_Register (SOCKET s, HTRequest * rq, SockOps ops,
			     HTEventCallBack *cbf, HTPriority p) 
{
    if (THD_TRACE) 
	fprintf(TDEST, "Register.... socket %d, request %p HTEventCallBack %p SockOps %x at priority %d\n",
		s, (void *)rq,  (void *)cbf, (unsigned) ops, (unsigned) p) ;


    (void)__HTEvent_addRequest( s, rq, ops, cbf, p);
 
#ifdef _WINSOCKAPI_
    if (rq -> hwnd != 0) {
        if (WSAAsyncSelect( s, rq->hwnd, rq->winMsg, ops) < 0) {
	    HTErrorSysAdd( rq, ERR_FATAL, GetLastError(), NO,"WSAAsyncSelect");
	    return HTERROR;
	}
    }
#endif
    /* insert socket into appropriate file descriptor set */

    if (ops & ReadBits)	{
	if (! FD_ISSET(s, &read_fds))
	    FD_SET(s, &read_fds) ;
	if (THD_TRACE)
	   fprintf(TDEST, "Register.... Registering socket as readable\n");
    }

    if (ops & WriteBits) {
	if (! FD_ISSET(s, &write_fds))
	    FD_SET(s, &write_fds);
	if (THD_TRACE) 
	   fprintf(TDEST, "Register.... Registering socket as writeable\n");
    }

    if (ops & ExceptBits) { 
	if (! FD_ISSET(s, &except_fds))
	    FD_SET(s, &except_fds);
	if (THD_TRACE) 
	    fprintf(TDEST, "Register.... Registering socket for exceptions\n");
    }

    if (!FD_ISSET(s, &all_fds)) {
	socketsInUse++;
	FD_SET(s, &all_fds);
    }
    if (s > max_sock) 
        max_sock = s ;
    
    return 0 ;
}

/*
**  HTEvent_addRequst - add the given socket and its associated Request
**  when socket has activity, the registered HTEventCallBack function will
**  invoked  
*/
PRIVATE int __HTEvent_addRequest(SOCKET s, HTRequest * rq, SockOps ops,
				 HTEventCallBack *cbf, HTPriority p)
{
    RQ * rqp = 0 , **rqpp = 0 ;
    long v = HASH(s);
    BOOL found = NO;
    for (rqpp = &table[v]; (rqp = *rqpp) != 0 ; rqpp = &rqp->next) {
        if (rqp->s == s) {
            __RequestUpdate( rqp, s, rq, ops, cbf, p) ;
            found = YES;
            break;
        }
    }  
    
    if (THD_TRACE)
    	fprintf(TDEST, "AddRequest.. %s socket %d\n", found ? "found" : "Did NOT find" , s) ;          

    if (!found) { 
        /* error if memory not allocated */
        *rqpp = rqp = (RQ *) calloc(1, sizeof(RQ));
	if (!rqp) outofmem(__FILE__, "__HTEvent_addRequest");
        __RequestInit( rqp, s, rq, ops, cbf, p) ;
    }
    return 0;
}


/*
** RequestInit - 
** initialize the request structure given 
** a set of inputs. N.B. This initializes the entire registration structure 
*/
PRIVATE void __RequestInit(RQ * rqp, SOCKET s, HTRequest * rq, 
			   SockOps ops, HTEventCallBack *cbf, HTPriority p) 
{
    if( THD_TRACE)
    	fprintf(TDEST,"RequestInit. initializing RQ entry for socket %d\n", s);
    rqp->s = s; 
    __RequestUpdate( rqp, s, rq, ops, cbf, p) ;
    rqp->next = 0 ;
    return ;
}  

/*
** RequestUpdate 
**
** updates the actions fields and the unregister value, but doesn't modify 
** the socket _or_ the next pointer
*/
PRIVATE void __RequestUpdate( RQ * rqp, SOCKET s, HTRequest * rq,
			     SockOps ops, HTEventCallBack * cbf, HTPriority p)
{
    if (THD_TRACE) 
    	fprintf(TDEST, "Req Update.. updating for socket %u\n", s) ;
    rqp->unregister = (ops & FD_UNREGISTER) ? YES : NO;
    rqp->actions[0].rq = rq ;
    rqp->actions[0].ops = ops ;
    rqp->actions[0].cbf = cbf ;
    rqp->actions[0].p = p ;
    return;
}

/*
** HTEvent_UnRegister
** remove the registered information for the specified socket for the actions 
** specified in ops. if no actions remain after the unregister, the registered
** info is deleted, and, if the socket has been registered for notification, 
** the HTEventCallBack will be invoked.
*/
PUBLIC int HTEvent_UnRegister( SOCKET s, SockOps ops) 
{
    long v = HASH(s) ;
    int rv = 0 ;
    RQ * rqp = 0, **rqpp = 0;
    int found = 0;

    for (rqpp = &table[v]; (rqp = *rqpp) != 0 ; rqpp = &rqp->next) {
        if (rqp -> s == s ) { 
            found = 1 ;
            break;
        }
    }

    if (THD_TRACE)
    	fprintf(TDEST, "UnRegister.. %s entry for socket %d\n",
		(found) ? "Found" : "Didn't find", s);
    if (! found) 
        return 0;

    rv = __EventUnregister( rqp, rqpp, ops) ;
    return rv ;
}

/*
** HTEvent_Retrieve 
** given a socket, return the HTEventCallBack function registered for it
** and return the HTRequest pointer associated with it.
** If the socket isn't found, the function returns NULL
*/
PUBLIC HTEventCallBack *HTEvent_Retrieve(SOCKET s, SockOps ops,HTRequest **arp)
{
    long value = HASH(s);
    register RQ * rqp = 0, **rqpp = 0 ;
    BOOL found = NO;

    *arp = 0 ;	  /* just too be sure */

    for (rqpp = &table[value]; (rqp = *rqpp) != 0 ; rqpp = &rqp->next) {
        if (rqp -> s == s ) { 
            found = YES;
            break;
        }
    }

    if (THD_TRACE) 
    	fprintf(TDEST, "Retrieve.... %s socket %d\n", found ? "Found" : "Didn't find", s) ;

    if (!found) 
        return (HTEventCallBack *) NULL;
    else {
    	*arp = rqp->actions[0].rq;
        return rqp->actions[0].cbf;
    }      
}

/*
** HTEvent_UnregisterAll
** unregister all sockets 
** N.B. we just remove them for our internal data structures: it is up to the 
** application to actually close the socket. 
*/
PUBLIC int HTEvent_UnregisterAll( void ) 
{
    int i ;
    register RQ * rqp, **rqpp ;

    /* begin */
    if (THD_TRACE)
	fprintf(TDEST, "Unregister.. all sockets\n");

    for (i = 0 ; i < PRIME_TABLE_SIZE; i++) {
	if (table[i] != 0) { 
            for (rqpp = &table[i]; (rqp = *rqpp) != 0 ; ) {
		(void)__EventUnregister(rqp, rqpp, FD_ALL) ;
            }        /* for each element on list */
	    table[i] = 0 ;
	} /* if table != 0 */
    }     /* for each element in table */

    max_sock = 0 ;

    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);
    FD_ZERO(&except_fds) ;
    FD_ZERO(&all_fds);

    return 0;
}


/*
** HTEvent_Loop
** event loop: that is, we wait for activity from one of our registered 
** channels, and dispatch on that.
**
** Under Windows/NT, we must treat the console and sockets as distinct. 
** That means we can't avoid a busy wait, but we do our best.
*/
PUBLIC int HTEvent_Loop( HTRequest * theRequest ) 
{
    fd_set treadset, twriteset, texceptset ;    
    int active_sockets;
    int maxfds ;
    int readings, writings, exceptions ;
    SOCKET s ;
    BOOL consoleReady = NO;
    int status = 0 ;
    unsigned ui ;

    /* Don't leave this loop until we the application */
    do { 
        treadset = read_fds;
        twriteset = write_fds ;
        texceptset = except_fds ;  
        maxfds = max_sock ; 
        readings = writings = exceptions = 0; 
	consoleReady = NO;

	if (console_in_use) { 

#ifdef _WIN32
            int result;
 	    DWORD time2wait;
	    tvptr = &seltime.tv;
	    tvptr->tv_sec = SecondsToWait;
	    tvptr->tv_usec = 0;
	    if (max_sock == 0)     /* no sockets, so wait for keyboard input */
		time2wait = INFINITE;
 	    else 
		time2wait = 1000;	      /* this is a poll - one second */

	    if (THD_TRACE)
		fprintf(TDEST, "Event Loop. console in use: waiting %s\n",
			(time2wait == 1000) ? "1 second" : "forever" );

	    switch( result = WaitForSingleObject( console_handle, time2wait)) {
		case WAIT_TIMEOUT:
			break;

		case WAIT_FAILED :
		case WAIT_ABANDONED: 		      /* should never happen */
		    HTErrorSysAdd(theRequest, ERR_FATAL, GetLastError(), NO,
				  "WaitForSingleObject");
		    return HT_ERROR;
		    break ; 

		case WAIT_OBJECT_0:
		    consoleReady = YES;
		    tvptr->tv_sec = 0 ;		    /* just poll the sockets */
		    break;
	     } /* switch */
	     if (THD_TRACE)
		fprintf(TDEST, "Console..... %s ready for input\n",
			consoleReady ? "is" : "ISN'T" );
#endif /* _WIN32 */

	} /* if tty in use */
	 
	/* if the console isn't ready, then this will block for 5 seconds;
	   otherwise, it's a poll
	 */

	if (THD_TRACE)
	    fprintf(TDEST, "Event Loop.. calling select: maxfds is %d\n",
		    maxfds);

#ifdef __hpux 
        active_sockets = select(maxfds+1, (int *)&treadset, (int *)&twriteset,
				(int *)&texceptset, (struct timeval *) tvptr);
#else
        active_sockets = select(maxfds+1, &treadset, &twriteset, &texceptset,
				(struct timeval *) tvptr);
#endif
	if (THD_TRACE)
	    fprintf(TDEST, "Event Loop.. select returns %d\n", active_sockets);

        switch(active_sockets)  {
            case 0:         /* no activity - timeout - allowed */
#ifndef _WIN32
	    
#endif /* WIN32 */
	    break;
            
            case -1:        /* error has occurred */
	    	HTErrorSysAdd( theRequest, ERR_FATAL, socerrno, NO, "select");
		__DumpFDSet( &treadset, "Read");
		__DumpFDSet( &twriteset, "Write") ;
		__DumpFDSet( &texceptset, "Exceptions");
                return HT_ERROR;
                break;
            
            default:
                break;
        } /* switch */

#ifndef _WINDOWS 
	if (console_in_use) {
	    consoleReady = active_sockets && 
		FD_ISSET((SOCKET) (console_handle), &treadset);
        }
#endif
	
	if (console_in_use && consoleReady) {
	    if (THD_TRACE) 
	    	fprintf(TDEST,"Event Loop.. console ready, invoke callback\n");
	    status = __DoUserCallBack((SOCKET) console_handle, FD_READ);
	    if (status != HT_OK)
		return status;

#ifndef _WINDOWS
	    /* Remember to take out the console handle from the read set */
	    FD_CLR((SOCKET) (console_handle), &treadset);
#endif
	    
	} /* if console in use and it's ready */

	/* If timeout then see if we should call callback function */
	if (active_sockets == 0) {
	    if (seltime.tcbf && (seltime.always || HTNet_activeCount())) {
		if (THD_TRACE)
		    fprintf(TDEST, "Event Loop.. calling timeout cbf\n");
		(*(seltime.tcbf))(seltime.request);
	    } else
		continue;
	}

	/*
	 * there were active sockets. Determine which fd sets they were in
	 */

#ifdef _WINSOCKAPI_ 
	for (ui = 0 ; ui < all_fds.fd_count; ui++) { 
            s = all_fds.fd_array[ui]; 
#else 
	for (s = 0 ; s <= maxfds ; s++) { 
#endif
	    { 
	        if (FD_ISSET(s, &texceptset))
		    exceptions++;
		if (FD_ISSET(s, &treadset))
		    readings++;
		if (FD_ISSET(s, &twriteset))
		    writings++;
	    }
	} /* for */

        if (exceptions) {
            if ((status= __ProcessFds(&texceptset, FD_OOB, "Except")) != HT_OK)
	    	return status;
        }

        if (writings) { 
            if ((status= __ProcessFds(&twriteset, FD_WRITE, "Write")) != HT_OK)
	    	return status;
        }

        if (readings) {
            if ((status = __ProcessFds(&treadset, FD_READ, "Read")) != HT_OK)
	    	return status;
        }

    } while(1);
}

/*
** ProcessFds 
** preform the associated HTEventCallBack function for each FD in a given set  
*/
PRIVATE int __ProcessFds( fd_set * fdsp, SockOps ops, CONST char * str) 
{
    unsigned ui ;
    SOCKET s ;
    if (THD_TRACE)
	fprintf(TDEST, "Processing.. %s socket set. max_sock is %d\n",
		str, max_sock);
    
#ifdef _WINSOCKAPI_ 
    for (ui = 0 ; ui < fdsp->fd_count; ui++) {
	s = fdsp->fd_array[ui] ;
#else
    for (s = 0 ; s <= max_sock; s++) {
        if (FD_ISSET( s, fdsp)) 
#endif 
	return __DoCallBack( s, ops);
    }
    return HT_OK;
}

/*
** __DoCallBack( SOCKET, SockOps ) 
**
** a  little function that just invokes the HTEventCallBack routine associated
** with the given socket. 
**
*/
PRIVATE int __DoCallBack( SOCKET s, SockOps ops)
{
    HTRequest * rqp = NULL;
    HTEventCallBack *cbf = HTEvent_Retrieve( s, ops, &rqp);
    /* although it makes no sense, callbacks can be null */
    return cbf ? cbf(s, rqp, ops) : 0;
}

/*
** __DoUserCallBack - invoke the callback function that the user requested. 
**
*/
PRIVATE int __DoUserCallBack( SOCKET s, SockOps ops)
{
    HTRequest * rqp = NULL;
    HTEventCallBack *cbf = HTEvent_Retrieve( s, ops, &rqp);
    /* although it makes no sense, callbacks can be null*/
    return cbf ? cbf(s, rqp, ops) : 0;
}

/*
** ResetMaxSock - reset the value of the maximum socket in use 
** 
*/
PRIVATE void __ResetMaxSock( void )
{
    unsigned  ui;
    SOCKET s ;
    SOCKET t_max = 0; 

    if (THD_TRACE)
    	fprintf(TDEST, "ResetMaxSock max socket is %u\n", max_sock);

#ifdef _WINSOCKAPI_ 
    for (ui = 0 ; ui < all_fds.fd_count; ui++) { 
        s = all_fds.fd_array[ui] ;
#else 
    for (s = 0 ; s <= max_sock; s++) { 
        if (FD_ISSET(s, &all_fds))
#endif
        {
  	    if (s > t_max)
 	        t_max = s ;
	} /* scope */
    } /* for */

    max_sock = t_max ;
    if (THD_TRACE)
    	fprintf(TDEST,"ResetMaxSock new max is %u\n", max_sock);
    return;
}  

PRIVATE int __EventUnregister(register RQ *rqp, register RQ ** rqpp,
			      SockOps ops) 
{
    register struct action_t * ap = &rqp->actions[0];
    SOCKET s = rqp->s ;
    int rv ;

    ap->ops &= ~ops ;      /* get rid of 'em */

    if (THD_TRACE)
    	fprintf(TDEST, "Unregister.. operations set for socket is %x\n",
		ap->ops);

    /* do we need to delete the socket from it's set as well? */

    if ((ap->ops & ReadBits) == 0)    /* nothing for reading */
        FD_CLR( s, &read_fds);
    if ((ap->ops & WriteBits) == 0)    /* nothing for writing */
        FD_CLR( s, &write_fds);
    if ((ap->ops & ExceptBits) == 0)   /* nothing for 'rithmetic */
        FD_CLR(s, &except_fds);


    /* if all actions are clear we are free to delete our request structure */
    if (ap->ops == 0)  {
        if (rqp->unregister) /* requested HTEventCallBack */
            rv = ap->cbf( s, ap->rq, FD_UNREGISTER); 
	FD_CLR(rqp->s, &all_fds) ;
	if (rqp->s == max_sock) 
	    	__ResetMaxSock();

#ifdef _WINSOCKAPI_
	if (ap->rq->hwnd) 
	    (void)WSAAsyncSelect( rqp->s, ap->rq->hwnd, 0, 0);
#endif
        *rqpp = rqp->next ;

	/* NB we only count network sockets, NOT the console/display socket */
	if (console_in_use && rqp->s != (SOCKET)console_handle) { 
	    if (socketsInUse-- == 0)
	    	socketsInUse = 0;
	}
        free(rqp) ;
    }  /* if all unregistered */
    else { 
#ifdef _WINSOCKAPI_
	if (ap->rq->hwnd != 0) { 			      /* re-register */
	    (void)WSAAsyncSelect(rqp->s, ap->rq->hwnd, ap->rq->winMsg,ap->ops);
	}
#endif /* _WINSOCKAPI_ */
    }
    return 0 ;
}

PRIVATE void __DumpFDSet( fd_set * fdp, CONST char * str) 
{
    SOCKET s ;
    unsigned ui ;

    
    if (THD_TRACE) {
	
	fprintf(TDEST, "Dumping..... %s file descriptor set\n", str );
#ifdef _WINSOCKAPI_ 
        for (ui = 0 ; ui < fdp->fd_count; ui++) { 
            s = all_fds.fd_array[ui] ;
#else 
        for (s = 0 ; s <= max_sock; s++) { 
            if (FD_ISSET(s, &all_fds))
#endif
	    {
	        fprintf(TDEST, "%4d\n", s);
	    }
        }	/* for */
    }           /* if */
    return ;
}
