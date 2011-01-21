/* ====================================================================
 * The Apache Software License, Version 1.1
 *
 * Copyright (c) 2000-2003 The Apache Software Foundation.  All rights
 * reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The end-user documentation included with the redistribution,
 *    if any, must include the following acknowledgment:
 *       "This product includes software developed by the
 *        Apache Software Foundation (http://www.apache.org/)."
 *    Alternately, this acknowledgment may appear in the software itself,
 *    if and wherever such third-party acknowledgments normally appear.
 *
 * 4. The names "Apache" and "Apache Software Foundation" must
 *    not be used to endorse or promote products derived from this
 *    software without prior written permission. For written
 *    permission, please contact apache@apache.org.
 *
 * 5. Products derived from this software may not be called "Apache",
 *    nor may "Apache" appear in their name, without prior written
 *    permission of the Apache Software Foundation.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE APACHE SOFTWARE FOUNDATION OR
 * ITS CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 * ====================================================================
 *
 * This software consists of voluntary contributions made by many
 * individuals on behalf of the Apache Software Foundation.  For more
 * information on the Apache Software Foundation, please see
 * <http://www.apache.org/>.
 *
 * Portions of this software are based upon public domain software
 * originally written at the National Center for Supercomputing Applications,
 * University of Illinois, Urbana-Champaign.
 */

#ifndef APACHE_MPM_DEFAULT_H
#define APACHE_MPM_DEFAULT_H

/* Number of processors to spawn off for each ServerEnvironment by default */

#ifndef DEFAULT_START_PROCESSORS
#define DEFAULT_START_PROCESSORS 1
#endif

/* Minimum number of running processors per ServerEnvironment */

#ifndef DEFAULT_MIN_PROCESSORS
#define DEFAULT_MIN_PROCESSORS 0
#endif

/* Minimum --- fewer than this, and more will be created */

#ifndef DEFAULT_MIN_FREE_PROCESSORS
#define DEFAULT_MIN_FREE_PROCESSORS 2
#endif

/* Maximum --- more than this, and idle processors will be killed (0 = disable) */

#ifndef DEFAULT_MAX_FREE_PROCESSORS
#define DEFAULT_MAX_FREE_PROCESSORS 0
#endif

/* Maximum processors per ServerEnvironment */

#ifndef DEFAULT_MAX_PROCESSORS
#define DEFAULT_MAX_PROCESSORS 10
#endif

/* File used for accept locking, when we use a file */
#ifndef DEFAULT_LOCKFILE
#define DEFAULT_LOCKFILE DEFAULT_REL_RUNTIMEDIR "/accept.lock"
#endif

/* Where the main/parent process's pid is logged */
#ifndef DEFAULT_PIDLOG
#define DEFAULT_PIDLOG DEFAULT_REL_RUNTIMEDIR "/httpd.pid"
#endif

/*
 * Interval, in microseconds, between scoreboard maintenance.
 */
#ifndef SCOREBOARD_MAINTENANCE_INTERVAL
#define SCOREBOARD_MAINTENANCE_INTERVAL 1000000
#endif

/* Number of requests to try to handle in a single process.  If <= 0,
 * the children don't die off.
 */
#ifndef DEFAULT_MAX_REQUESTS_PER_CHILD
#define DEFAULT_MAX_REQUESTS_PER_CHILD 10000
#endif

/* Maximum multiplexers */

#ifndef DEFAULT_MAX_MULTIPLEXERS
#define DEFAULT_MAX_MULTIPLEXERS 20
#endif

/* Minimum multiplexers */

#ifndef DEFAULT_MIN_MULTIPLEXERS
#define DEFAULT_MIN_MULTIPLEXERS 3
#endif

/* Amount of time a child can run before it expires (0 = turn off) */

#ifndef DEFAULT_EXPIRE_TIMEOUT
#define DEFAULT_EXPIRE_TIMEOUT 1800
#endif

/* Amount of time a child can stay idle (0 = turn off) */

#ifndef DEFAULT_IDLE_TIMEOUT
#define DEFAULT_IDLE_TIMEOUT 900
#endif

/* Amount of time a multiplexer can stay idle (0 = turn off) */

#ifndef DEFAULT_MULTIPLEXER_IDLE_TIMEOUT
#define DEFAULT_MULTIPLEXER_IDLE_TIMEOUT 0
#endif

/* Amount of maximum time a multiplexer can wait for processor if it is busy (0 = never wait)
 * This is decreased with every busy request
 */

#ifndef DEFAULT_PROCESSOR_WAIT_TIMEOUT
#define DEFAULT_PROCESSOR_WAIT_TIMEOUT 5
#endif

/* The number of different levels there are when a multiplexer is waiting for processor
 * (between maximum waiting time and no waiting)
 */

#ifndef DEFAULT_PROCESSOR_WAIT_STEPS
#define DEFAULT_PROCESSOR_WAIT_STEPS 10
#endif

/*
 * Max %CPU for each processor (0 to disable)
 */
 
#ifndef DEFAULT_MAX_CPU_USAGE
#define DEFAULT_MAX_CPU_USAGE 0
#endif

/* 
 * Header buffer used between pass_request() and receive_connection()
 */

#ifndef PERUSER_MAX_HEADER_SIZE
#define PERUSER_MAX_HEADER_SIZE 20000
#endif

#endif /* AP_MPM_DEFAULT_H */
