/*
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "redis.h"
#include "slowlog.h"
#include "bio.h"

#ifdef HAVE_BACKTRACE
#include <execinfo.h>
#include <ucontext.h>
#endif /* HAVE_BACKTRACE */

#include <time.h>
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <assert.h>
#include <ctype.h>
#include <stdarg.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <limits.h>
#include <float.h>
#include <math.h>
#include <sys/resource.h>
#include "redislite.h"
#include "public_api.h"
#include "memory.h"

/* Our shared "common" objects */

struct sharedObjectsStruct shared;

/* Global vars that are actually used as constants. The following double
 * values are used for double on-disk serialization, and are initialized
 * at runtime to avoid strange compiler optimizations. */

double R_Zero, R_PosInf, R_NegInf, R_Nan;

/*================================= Globals ================================= */

/* Global vars */
struct redisServer server; /* server global state */
struct redisCommand *commandTable;

/* Our command table.
 *
 * Every entry is composed of the following fields:
 *
 * name: a string representing the command name.
 * function: pointer to the C function implementing the command.
 * arity: number of arguments, it is possible to use -N to say >= N
 * sflags: command flags as string. See below for a table of flags.
 * flags: flags as bitmask. Computed by Redis using the 'sflags' field.
 * get_keys_proc: an optional function to get key arguments from a command.
 *                This is only used when the following three fields are not
 *                enough to specify what arguments are keys.
 * first_key_index: first argument that is a key
 * last_key_index: last argument that is a key
 * key_step: step to get all the keys from first to last argument. For instance
 *           in MSET the step is two since arguments are key,val,key,val,...
 * microseconds: microseconds of total execution time for this command.
 * calls: total number of calls of this command.
 *
 * The flags, microseconds and calls fields are computed by Redis and should
 * always be set to zero.
 *
 * Command flags are expressed using strings where every character represents
 * a flag. Later the populateCommandTable() function will take care of
 * populating the real 'flags' field using this characters.
 *
 * This is the meaning of the flags:
 *
 * w: write command (may modify the key space).
 * r: read command  (will never modify the key space).
 * m: may increase memory usage once called. Don't allow if out of memory.
 * a: admin command, like SAVE or SHUTDOWN.
 * p: Pub/Sub related command.
 * f: force replication of this command, regarless of server.dirty.
 * s: command not allowed in scripts.
 * R: random command. Command is not deterministic, that is, the same command
 *    with the same arguments, with the same key space, may have different
 *    results. For instance SPOP and RANDOMKEY are two random commands. */
struct redisCommand redisCommandTable[] = {
    {"get",getCommand,2,"r",0,NULL,1,1,1,0,0},
    {"set",setCommand,3,"wm",0,noPreloadGetKeys,1,1,1,0,0},
    {"setnx",setnxCommand,3,"wm",0,noPreloadGetKeys,1,1,1,0,0},
    {"setex",setexCommand,4,"wm",0,noPreloadGetKeys,1,1,1,0,0},
    {"psetex",psetexCommand,4,"wm",0,noPreloadGetKeys,1,1,1,0,0},
    {"append",appendCommand,3,"wm",0,NULL,1,1,1,0,0},
    {"strlen",strlenCommand,2,"r",0,NULL,1,1,1,0,0},
    {"del",delCommand,-2,"w",0,noPreloadGetKeys,1,-1,1,0,0},
    {"exists",existsCommand,2,"r",0,NULL,1,1,1,0,0},
    {"setbit",setbitCommand,4,"wm",0,NULL,1,1,1,0,0},
    {"getbit",getbitCommand,3,"r",0,NULL,1,1,1,0,0},
    {"setrange",setrangeCommand,4,"wm",0,NULL,1,1,1,0,0},
    {"getrange",getrangeCommand,4,"r",0,NULL,1,1,1,0,0},
    {"substr",getrangeCommand,4,"r",0,NULL,1,1,1,0,0},
    {"incr",incrCommand,2,"wm",0,NULL,1,1,1,0,0},
    {"decr",decrCommand,2,"wm",0,NULL,1,1,1,0,0},
    {"mget",mgetCommand,-2,"r",0,NULL,1,-1,1,0,0},
    {"rpush",rpushCommand,-3,"wm",0,NULL,1,1,1,0,0},
    {"lpush",lpushCommand,-3,"wm",0,NULL,1,1,1,0,0},
    {"rpushx",rpushxCommand,3,"wm",0,NULL,1,1,1,0,0},
    {"lpushx",lpushxCommand,3,"wm",0,NULL,1,1,1,0,0},
    {"linsert",linsertCommand,5,"wm",0,NULL,1,1,1,0,0},
    {"rpop",rpopCommand,2,"w",0,NULL,1,1,1,0,0},
    {"lpop",lpopCommand,2,"w",0,NULL,1,1,1,0,0},
    {"brpop",brpopCommand,-3,"ws",0,NULL,1,1,1,0,0},
    {"brpoplpush",brpoplpushCommand,4,"wms",0,NULL,1,2,1,0,0},
    {"blpop",blpopCommand,-3,"ws",0,NULL,1,-2,1,0,0},
    {"llen",llenCommand,2,"r",0,NULL,1,1,1,0,0},
    {"lindex",lindexCommand,3,"r",0,NULL,1,1,1,0,0},
    {"lset",lsetCommand,4,"wm",0,NULL,1,1,1,0,0},
    {"lrange",lrangeCommand,4,"r",0,NULL,1,1,1,0,0},
    {"ltrim",ltrimCommand,4,"w",0,NULL,1,1,1,0,0},
    {"lrem",lremCommand,4,"w",0,NULL,1,1,1,0,0},
    {"rpoplpush",rpoplpushCommand,3,"wm",0,NULL,1,2,1,0,0},
    {"sadd",saddCommand,-3,"wm",0,NULL,1,1,1,0,0},
    {"srem",sremCommand,-3,"w",0,NULL,1,1,1,0,0},
    {"smove",smoveCommand,4,"w",0,NULL,1,2,1,0,0},
    {"sismember",sismemberCommand,3,"r",0,NULL,1,1,1,0,0},
    {"scard",scardCommand,2,"r",0,NULL,1,1,1,0,0},
    {"spop",spopCommand,2,"wRs",0,NULL,1,1,1,0,0},
    {"srandmember",srandmemberCommand,2,"rR",0,NULL,1,1,1,0,0},
    {"sinter",sinterCommand,-2,"r",0,NULL,1,-1,1,0,0},
    {"sinterstore",sinterstoreCommand,-3,"wm",0,NULL,1,-1,1,0,0},
    {"sunion",sunionCommand,-2,"r",0,NULL,1,-1,1,0,0},
    {"sunionstore",sunionstoreCommand,-3,"wm",0,NULL,1,-1,1,0,0},
    {"sdiff",sdiffCommand,-2,"r",0,NULL,1,-1,1,0,0},
    {"sdiffstore",sdiffstoreCommand,-3,"wm",0,NULL,1,-1,1,0,0},
    {"smembers",sinterCommand,2,"r",0,NULL,1,1,1,0,0},
    {"zadd",zaddCommand,-4,"wm",0,NULL,1,1,1,0,0},
    {"zincrby",zincrbyCommand,4,"wm",0,NULL,1,1,1,0,0},
    {"zrem",zremCommand,-3,"w",0,NULL,1,1,1,0,0},
    {"zremrangebyscore",zremrangebyscoreCommand,4,"w",0,NULL,1,1,1,0,0},
    {"zremrangebyrank",zremrangebyrankCommand,4,"w",0,NULL,1,1,1,0,0},
    {"zunionstore",zunionstoreCommand,-4,"wm",0,zunionInterGetKeys,0,0,0,0,0},
    {"zinterstore",zinterstoreCommand,-4,"wm",0,zunionInterGetKeys,0,0,0,0,0},
    {"zrange",zrangeCommand,-4,"r",0,NULL,1,1,1,0,0},
    {"zrangebyscore",zrangebyscoreCommand,-4,"r",0,NULL,1,1,1,0,0},
    {"zrevrangebyscore",zrevrangebyscoreCommand,-4,"r",0,NULL,1,1,1,0,0},
    {"zcount",zcountCommand,4,"r",0,NULL,1,1,1,0,0},
    {"zrevrange",zrevrangeCommand,-4,"r",0,NULL,1,1,1,0,0},
    {"zcard",zcardCommand,2,"r",0,NULL,1,1,1,0,0},
    {"zscore",zscoreCommand,3,"r",0,NULL,1,1,1,0,0},
    {"zrank",zrankCommand,3,"r",0,NULL,1,1,1,0,0},
    {"zrevrank",zrevrankCommand,3,"r",0,NULL,1,1,1,0,0},
    {"hset",hsetCommand,4,"wm",0,NULL,1,1,1,0,0},
    {"hsetnx",hsetnxCommand,4,"wm",0,NULL,1,1,1,0,0},
    {"hget",hgetCommand,3,"r",0,NULL,1,1,1,0,0},
    {"hmset",hmsetCommand,-4,"wm",0,NULL,1,1,1,0,0},
    {"hmget",hmgetCommand,-3,"r",0,NULL,1,1,1,0,0},
    {"hincrby",hincrbyCommand,4,"wm",0,NULL,1,1,1,0,0},
    {"hincrbyfloat",hincrbyfloatCommand,4,"wm",0,NULL,1,1,1,0,0},
    {"hdel",hdelCommand,-3,"w",0,NULL,1,1,1,0,0},
    {"hlen",hlenCommand,2,"r",0,NULL,1,1,1,0,0},
    {"hkeys",hkeysCommand,2,"r",0,NULL,1,1,1,0,0},
    {"hvals",hvalsCommand,2,"r",0,NULL,1,1,1,0,0},
    {"hgetall",hgetallCommand,2,"r",0,NULL,1,1,1,0,0},
    {"hexists",hexistsCommand,3,"r",0,NULL,1,1,1,0,0},
    {"incrby",incrbyCommand,3,"wm",0,NULL,1,1,1,0,0},
    {"decrby",decrbyCommand,3,"wm",0,NULL,1,1,1,0,0},
    {"incrbyfloat",incrbyfloatCommand,3,"wm",0,NULL,1,1,1,0,0},
    {"getset",getsetCommand,3,"wm",0,NULL,1,1,1,0,0},
    {"mset",msetCommand,-3,"wm",0,NULL,1,-1,2,0,0},
    {"msetnx",msetnxCommand,-3,"wm",0,NULL,1,-1,2,0,0},
    {"randomkey",randomkeyCommand,1,"rR",0,NULL,0,0,0,0,0},
    {"select",selectCommand,2,"r",0,NULL,0,0,0,0,0},
    {"move",moveCommand,3,"w",0,NULL,1,1,1,0,0},
    {"rename",renameCommand,3,"w",0,renameGetKeys,1,2,1,0,0},
    {"renamenx",renamenxCommand,3,"w",0,renameGetKeys,1,2,1,0,0},
    {"expire",expireCommand,3,"w",0,NULL,1,1,1,0,0},
    {"expireat",expireatCommand,3,"w",0,NULL,1,1,1,0,0},
    {"pexpire",pexpireCommand,3,"w",0,NULL,1,1,1,0,0},
    {"pexpireat",pexpireatCommand,3,"w",0,NULL,1,1,1,0,0},
    {"keys",keysCommand,2,"r",0,NULL,0,0,0,0,0},
    {"dbsize",dbsizeCommand,1,"r",0,NULL,0,0,0,0,0},
    {"auth",authCommand,2,"rs",0,NULL,0,0,0,0,0},
    {"ping",pingCommand,1,"r",0,NULL,0,0,0,0,0},
    {"echo",echoCommand,2,"r",0,NULL,0,0,0,0,0},
    {"save",saveCommand,1,"ars",0,NULL,0,0,0,0,0},
    {"bgsave",bgsaveCommand,1,"ar",0,NULL,0,0,0,0,0},
    {"bgrewriteaof",bgrewriteaofCommand,1,"ar",0,NULL,0,0,0,0,0},
    {"shutdown",shutdownCommand,-1,"ar",0,NULL,0,0,0,0,0},
    {"lastsave",lastsaveCommand,1,"r",0,NULL,0,0,0,0,0},
    {"type",typeCommand,2,"r",0,NULL,1,1,1,0,0},
    {"multi",multiCommand,1,"rs",0,NULL,0,0,0,0,0},
    {"exec",execCommand,1,"wms",0,NULL,0,0,0,0,0},
    {"discard",discardCommand,1,"rs",0,NULL,0,0,0,0,0},
    {"sync",syncCommand,1,"ars",0,NULL,0,0,0,0,0},
    {"flushdb",flushdbCommand,1,"w",0,NULL,0,0,0,0,0},
    {"flushall",flushallCommand,1,"w",0,NULL,0,0,0,0,0},
    {"sort",sortCommand,-2,"wm",0,NULL,1,1,1,0,0},
    {"info",infoCommand,-1,"r",0,NULL,0,0,0,0,0},
    {"monitor",monitorCommand,1,"ars",0,NULL,0,0,0,0,0},
    {"ttl",ttlCommand,2,"r",0,NULL,1,1,1,0,0},
    {"pttl",pttlCommand,2,"r",0,NULL,1,1,1,0,0},
    {"persist",persistCommand,2,"w",0,NULL,1,1,1,0,0},
    {"slaveof",slaveofCommand,3,"aws",0,NULL,0,0,0,0,0},
    {"debug",debugCommand,-2,"aws",0,NULL,0,0,0,0,0},
    {"config",configCommand,-2,"ar",0,NULL,0,0,0,0,0},
    {"subscribe",subscribeCommand,-2,"rps",0,NULL,0,0,0,0,0},
    {"unsubscribe",unsubscribeCommand,-1,"rps",0,NULL,0,0,0,0,0},
    {"psubscribe",psubscribeCommand,-2,"rps",0,NULL,0,0,0,0,0},
    {"punsubscribe",punsubscribeCommand,-1,"rps",0,NULL,0,0,0,0,0},
    {"publish",publishCommand,3,"rpf",0,NULL,0,0,0,0,0},
    {"watch",watchCommand,-2,"rs",0,noPreloadGetKeys,1,-1,1,0,0},
    {"unwatch",unwatchCommand,1,"rs",0,NULL,0,0,0,0,0},
    {"cluster",clusterCommand,-2,"ar",0,NULL,0,0,0,0,0},
    {"restore",restoreCommand,4,"awm",0,NULL,1,1,1,0,0},
    {"migrate",migrateCommand,6,"aw",0,NULL,0,0,0,0,0},
    {"asking",askingCommand,1,"r",0,NULL,0,0,0,0,0},
    {"dump",dumpCommand,2,"ar",0,NULL,1,1,1,0,0},
    {"object",objectCommand,-2,"r",0,NULL,2,2,2,0,0},
    {"client",clientCommand,-2,"ar",0,NULL,0,0,0,0,0},
    {"eval",evalCommand,-3,"wms",0,zunionInterGetKeys,0,0,0,0,0},
    {"evalsha",evalShaCommand,-3,"wms",0,zunionInterGetKeys,0,0,0,0,0},
    {"slowlog",slowlogCommand,-2,"r",0,NULL,0,0,0,0,0},
    {"script",scriptCommand,-2,"ras",0,NULL,0,0,0,0,0}
};

/*============================ Utility functions ============================ */

/* Low level logging. To use only for very big messages, otherwise
 * redisLog() is to prefer. */
void redisLogRaw(int level, const char *msg) {
    const int syslogLevelMap[] = { LOG_DEBUG, LOG_INFO, LOG_NOTICE, LOG_WARNING };
    const char *c = ".-*#";
    time_t now = time(NULL);
    FILE *fp;
    char buf[64];
    int rawmode = (level & REDIS_LOG_RAW);

    level &= 0xff; /* clear flags */
    if (level < server.verbosity) return;

    fp = (server.logfile == NULL) ? stdout : fopen(server.logfile,"a");
    if (!fp) return;

    if (rawmode) {
        fprintf(fp,"%s",msg);
    } else {
        strftime(buf,sizeof(buf),"%d %b %H:%M:%S",localtime(&now));
        fprintf(fp,"[%d] %s %c %s\n",(int)getpid(),buf,c[level],msg);
    }
    fflush(fp);

    if (server.logfile) fclose(fp);

    if (server.syslog_enabled) syslog(syslogLevelMap[level], "%s", msg);
}

/* Like redisLogRaw() but with printf-alike support. This is the funciton that
 * is used across the code. The raw version is only used in order to dump
 * the INFO output on crash. */
void redisLog(int level, const char *fmt, ...) {
    va_list ap;
    char msg[REDIS_MAX_LOGMSG_LEN];

    if ((level&0xff) < server.verbosity) return;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    redisLogRaw(level,msg);
}

/* Redis generally does not try to recover from out of memory conditions
 * when allocating objects or strings, it is not clear if it will be possible
 * to report this condition to the client since the networking layer itself
 * is based on heap allocation for send buffers, so we simply abort.
 * At least the code will be simpler to read... */
void oom(const char *msg) {
    redisLog(REDIS_WARNING, "%s: Out of memory\n",msg);
    sleep(1);
    abort();
}

/* Return the UNIX time in microseconds */
long long ustime(void) {
    struct timeval tv;
    long long ust;

    gettimeofday(&tv, NULL);
    ust = ((long long)tv.tv_sec)*1000000;
    ust += tv.tv_usec;
    return ust;
}

/* Return the UNIX time in milliseconds */
long long mstime(void) {
    return ustime()/1000;
}

/*====================== Hash table type implementation  ==================== */

/* This is an hash table type that uses the SDS dynamic strings libary as
 * keys and radis objects as values (objects can hold SDS strings,
 * lists, sets). */

void dictVanillaFree(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    zfree(val);
}

void dictListDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);
    listRelease((list*)val);
}

int dictSdsKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    int l1,l2;
    DICT_NOTUSED(privdata);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

/* A case insensitive version used for the command lookup table. */
int dictSdsKeyCaseCompare(void *privdata, const void *key1,
        const void *key2)
{
    DICT_NOTUSED(privdata);

    return strcasecmp(key1, key2) == 0;
}

void dictRedisObjectDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    if (val == NULL) return; /* Values of swapped out keys as set to NULL */
    decrRefCount(val);
}

void dictSdsDestructor(void *privdata, void *val)
{
    DICT_NOTUSED(privdata);

    sdsfree(val);
}

int dictObjKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    const robj *o1 = key1, *o2 = key2;
    return dictSdsKeyCompare(privdata,o1->ptr,o2->ptr);
}

unsigned int dictObjHash(const void *key) {
    const robj *o = key;
    return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
}

unsigned int dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}

unsigned int dictSdsCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char*)key, sdslen((char*)key));
}

int dictEncObjKeyCompare(void *privdata, const void *key1,
        const void *key2)
{
    robj *o1 = (robj*) key1, *o2 = (robj*) key2;
    int cmp;

    if (o1->encoding == REDIS_ENCODING_INT &&
        o2->encoding == REDIS_ENCODING_INT)
            return o1->ptr == o2->ptr;

    o1 = getDecodedObject(o1);
    o2 = getDecodedObject(o2);
    cmp = dictSdsKeyCompare(privdata,o1->ptr,o2->ptr);
    decrRefCount(o1);
    decrRefCount(o2);
    return cmp;
}

unsigned int dictEncObjHash(const void *key) {
    robj *o = (robj*) key;

    if (o->encoding == REDIS_ENCODING_RAW) {
        return dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
    } else {
        if (o->encoding == REDIS_ENCODING_INT) {
            char buf[32];
            int len;

            len = ll2string(buf,32,(long)o->ptr);
            return dictGenHashFunction((unsigned char*)buf, len);
        } else {
            unsigned int hash;

            o = getDecodedObject(o);
            hash = dictGenHashFunction(o->ptr, sdslen((sds)o->ptr));
            decrRefCount(o);
            return hash;
        }
    }
}

/* Sets type hash table */
dictType setDictType = {
    dictEncObjHash,            /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictEncObjKeyCompare,      /* key compare */
    dictRedisObjectDestructor, /* key destructor */
    NULL                       /* val destructor */
};

/* Sorted sets hash (note: a skiplist is used in addition to the hash table) */
dictType zsetDictType = {
    dictEncObjHash,            /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictEncObjKeyCompare,      /* key compare */
    dictRedisObjectDestructor, /* key destructor */
    NULL                       /* val destructor */
};

/* Db->dict, keys are sds strings, vals are Redis objects. */
dictType dbDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    dictSdsDestructor,          /* key destructor */
    dictRedisObjectDestructor   /* val destructor */
};

/* Db->expires */
dictType keyptrDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    NULL,                      /* key destructor */
    NULL                       /* val destructor */
};

/* Command table. sds string -> command struct pointer. */
dictType commandTableDictType = {
    dictSdsCaseHash,           /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCaseCompare,     /* key compare */
    dictSdsDestructor,         /* key destructor */
    NULL                       /* val destructor */
};

/* Hash type hash table (note that small hashes are represented with zimpaps) */
dictType hashDictType = {
    dictEncObjHash,             /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictEncObjKeyCompare,       /* key compare */
    dictRedisObjectDestructor,  /* key destructor */
    dictRedisObjectDestructor   /* val destructor */
};

/* Keylist hash table type has unencoded redis objects as keys and
 * lists as values. It's used for blocking operations (BLPOP) and to
 * map swapped keys to a list of clients waiting for this keys to be loaded. */
dictType keylistDictType = {
    dictObjHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictObjKeyCompare,          /* key compare */
    dictRedisObjectDestructor,  /* key destructor */
    dictListDestructor          /* val destructor */
};

/* Cluster nodes hash table, mapping nodes addresses 1.2.3.4:6379 to
 * clusterNode structures. */
dictType clusterNodesDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    dictSdsDestructor,          /* key destructor */
    NULL                        /* val destructor */
};

int htNeedsResize(dict *dict) {
    long long size, used;

    size = dictSlots(dict);
    used = dictSize(dict);
    return (size && used && size > DICT_HT_INITIAL_SIZE &&
            (used*100/size < REDIS_HT_MINFILL));
}

/* If the percentage of used slots in the HT reaches REDIS_HT_MINFILL
 * we resize the hash table to save memory */
void tryResizeHashTables(void) {
    int j;

    for (j = 0; j < server.dbnum; j++) {
        if (htNeedsResize(server.db[j].dict))
            dictResize(server.db[j].dict);
        if (htNeedsResize(server.db[j].expires))
            dictResize(server.db[j].expires);
    }
}

/* Our hash table implementation performs rehashing incrementally while
 * we write/read from the hash table. Still if the server is idle, the hash
 * table will use two tables for a long time. So we try to use 1 millisecond
 * of CPU time at every serverCron() loop in order to rehash some key. */
void incrementallyRehash(void) {
    int j;

    for (j = 0; j < server.dbnum; j++) {
        if (dictIsRehashing(server.db[j].dict)) {
            dictRehashMilliseconds(server.db[j].dict,1);
            break; /* already used our millisecond for this loop... */
        }
    }
}

/* This function is called once a background process of some kind terminates,
 * as we want to avoid resizing the hash tables when there is a child in order
 * to play well with copy-on-write (otherwise when a resize happens lots of
 * memory pages are copied). The goal of this function is to update the ability
 * for dict.c to resize the hash tables accordingly to the fact we have o not
 * running childs. */
void updateDictResizePolicy(void) {
    if (server.bgsavechildpid == -1 && server.bgrewritechildpid == -1)
        dictEnableResize();
    else
        dictDisableResize();
}

/* ======================= Cron: called every 100 ms ======================== */

/* Try to expire a few timed out keys. The algorithm used is adaptive and
 * will use few CPU cycles if there are few expiring keys, otherwise
 * it will get more aggressive to avoid that too much memory is used by
 * keys that can be removed from the keyspace. */
void activeExpireCycle(void) {
    int j;

    for (j = 0; j < server.dbnum; j++) {
        int expired;
        redisDb *db = server.db+j;

        /* Continue to expire if at the end of the cycle more than 25%
         * of the keys were expired. */
        do {
            long num = dictSize(db->expires);
            long long now = mstime();

            expired = 0;
            if (num > REDIS_EXPIRELOOKUPS_PER_CRON)
                num = REDIS_EXPIRELOOKUPS_PER_CRON;
            while (num--) {
                dictEntry *de;
                long long t;

                if ((de = dictGetRandomKey(db->expires)) == NULL) break;
                t = dictGetSignedIntegerVal(de);
                if (now > t) {
                    sds key = dictGetKey(de);
                    robj *keyobj = createStringObject(key,sdslen(key));

                    propagateExpire(db,keyobj);
                    dbDelete(db,keyobj);
                    decrRefCount(keyobj);
                    expired++;
                    server.stat_expiredkeys++;
                }
            }
        } while (expired > REDIS_EXPIRELOOKUPS_PER_CRON/4);
    }
}

void updateLRUClock(void) {
    server.lruclock = (time(NULL)/REDIS_LRU_CLOCK_RESOLUTION) &
                                                REDIS_LRU_CLOCK_MAX;
}

int serverCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    int j, loops = server.cronloops;
    REDIS_NOTUSED(eventLoop);
    REDIS_NOTUSED(id);
    REDIS_NOTUSED(clientData);

    /* We take a cached value of the unix time in the global state because
     * with virtual memory and aging there is to store the current time
     * in objects at every object access, and accuracy is not needed.
     * To access a global var is faster than calling time(NULL) */
    server.unixtime = time(NULL);

    /* We have just 22 bits per object for LRU information.
     * So we use an (eventually wrapping) LRU clock with 10 seconds resolution.
     * 2^22 bits with 10 seconds resoluton is more or less 1.5 years.
     *
     * Note that even if this will wrap after 1.5 years it's not a problem,
     * everything will still work but just some object will appear younger
     * to Redis. But for this to happen a given object should never be touched
     * for 1.5 years.
     *
     * Note that you can change the resolution altering the
     * REDIS_LRU_CLOCK_RESOLUTION define.
     */
    updateLRUClock();

    /* Record the max memory used since the server was started. */
    if (zmalloc_used_memory() > server.stat_peak_memory)
        server.stat_peak_memory = zmalloc_used_memory();

    /* We received a SIGTERM, shutting down here in a safe way, as it is
     * not ok doing so inside the signal handler. */
    if (server.shutdown_asap) {
        if (prepareForShutdown(0) == REDIS_OK) exit(0);
        redisLog(REDIS_WARNING,"SIGTERM received but errors trying to shut down the server, check the logs for more information");
    }

    /* Show some info about non-empty databases */
    for (j = 0; j < server.dbnum; j++) {
        long long size, used, vkeys;

        size = dictSlots(server.db[j].dict);
        used = dictSize(server.db[j].dict);
        vkeys = dictSize(server.db[j].expires);
        if (!(loops % 50) && (used || vkeys)) {
            redisLog(REDIS_VERBOSE,"DB %d: %lld keys (%lld volatile) in %lld slots HT.",j,used,vkeys,size);
            /* dictPrintStats(server.dict); */
        }
    }

    /* We don't want to resize the hash tables while a bacground saving
     * is in progress: the saving child is created using fork() that is
     * implemented with a copy-on-write semantic in most modern systems, so
     * if we resize the HT while there is the saving child at work actually
     * a lot of memory movements in the parent will cause a lot of pages
     * copied. */
    if (server.bgsavechildpid == -1 && server.bgrewritechildpid == -1) {
        if (!(loops % 10)) tryResizeHashTables();
        if (server.activerehashing) incrementallyRehash();
    }

    /* Show information about connected clients */
    if (!(loops % 50)) {
        redisLog(REDIS_VERBOSE,"%d clients connected (%d slaves), %zu bytes in use",
            listLength(server.clients)-listLength(server.slaves),
            listLength(server.slaves),
            zmalloc_used_memory());
    }

    /* Close connections of timedout clients */
    if ((server.maxidletime && !(loops % 100)) || server.bpop_blocked_clients)
        closeTimedoutClients();

    /* Start a scheduled AOF rewrite if this was requested by the user while
     * a BGSAVE was in progress. */
    if (server.bgsavechildpid == -1 && server.bgrewritechildpid == -1 &&
        server.aofrewrite_scheduled)
    {
        rewriteAppendOnlyFileBackground();
    }

    /* Check if a background saving or AOF rewrite in progress terminated. */
    if (server.bgsavechildpid != -1 || server.bgrewritechildpid != -1) {
        int statloc;
        pid_t pid;

        if ((pid = wait3(&statloc,WNOHANG,NULL)) != 0) {
            int exitcode = WEXITSTATUS(statloc);
            int bysignal = 0;

            if (WIFSIGNALED(statloc)) bysignal = WTERMSIG(statloc);

            if (pid == server.bgsavechildpid) {
                backgroundSaveDoneHandler(exitcode,bysignal);
            } else {
                backgroundRewriteDoneHandler(exitcode,bysignal);
            }
            updateDictResizePolicy();
        }
    } else {
         time_t now = time(NULL);

        /* If there is not a background saving/rewrite in progress check if
         * we have to save/rewrite now */
         for (j = 0; j < server.saveparamslen; j++) {
            struct saveparam *sp = server.saveparams+j;

            if (server.dirty >= sp->changes &&
                now-server.lastsave > sp->seconds) {
                redisLog(REDIS_NOTICE,"%d changes in %d seconds. Saving...",
                    sp->changes, sp->seconds);
                rdbSaveBackground(server.dbfilename);
                break;
            }
         }

         /* Trigger an AOF rewrite if needed */
         if (server.bgsavechildpid == -1 &&
             server.bgrewritechildpid == -1 &&
             server.auto_aofrewrite_perc &&
             server.appendonly_current_size > server.auto_aofrewrite_min_size)
         {
            long long base = server.auto_aofrewrite_base_size ?
                            server.auto_aofrewrite_base_size : 1;
            long long growth = (server.appendonly_current_size*100/base) - 100;
            if (growth >= server.auto_aofrewrite_perc) {
                redisLog(REDIS_NOTICE,"Starting automatic rewriting of AOF on %lld%% growth",growth);
                rewriteAppendOnlyFileBackground();
            }
         }
    }


    /* If we postponed an AOF buffer flush, let's try to do it every time the
     * cron function is called. */
    if (server.aof_flush_postponed_start) flushAppendOnlyFile(0);

    /* Expire a few keys per cycle, only if this is a master.
     * On slaves we wait for DEL operations synthesized by the master
     * in order to guarantee a strict consistency. */
    if (server.masterhost == NULL) activeExpireCycle();

    /* Replication cron function -- used to reconnect to master and
     * to detect transfer failures. */
    if (!(loops % 10)) replicationCron();

    /* Run other sub-systems specific cron jobs */
    if (server.cluster_enabled && !(loops % 10)) clusterCron();

    server.cronloops++;
    return 100;
}

/* This function gets called every time Redis is entering the
 * main loop of the event driven library, that is, before to sleep
 * for ready file descriptors. */
void beforeSleep(struct aeEventLoop *eventLoop) {
    REDIS_NOTUSED(eventLoop);
    listNode *ln;
    redisClient *c;

    /* Try to process pending commands for clients that were just unblocked. */
    while (listLength(server.unblocked_clients)) {
        ln = listFirst(server.unblocked_clients);
        redisAssert(ln != NULL);
        c = ln->value;
        listDelNode(server.unblocked_clients,ln);
        c->flags &= ~REDIS_UNBLOCKED;

        /* Process remaining data in the input buffer. */
        if (c->querybuf && sdslen(c->querybuf) > 0)
            processInputBuffer(c);
    }

    /* Write the AOF buffer on disk */
    flushAppendOnlyFile(0);
}

/* =========================== Server initialization ======================== */

void createSharedObjects(void) {
    int j;

    shared.crlf = createObject(REDIS_STRING,sdsnew("\r\n"));
    shared.ok = createObject(REDIS_STRING,sdsnew("+OK\r\n"));
    shared.err = createObject(REDIS_STRING,sdsnew("-ERR\r\n"));
    shared.emptybulk = createObject(REDIS_STRING,sdsnew("$0\r\n\r\n"));
    shared.czero = createObject(REDIS_STRING,sdsnew(":0\r\n"));
    shared.cone = createObject(REDIS_STRING,sdsnew(":1\r\n"));
    shared.cnegone = createObject(REDIS_STRING,sdsnew(":-1\r\n"));
    shared.nullbulk = createObject(REDIS_STRING,sdsnew("$-1\r\n"));
    shared.nullmultibulk = createObject(REDIS_STRING,sdsnew("*-1\r\n"));
    shared.emptymultibulk = createObject(REDIS_STRING,sdsnew("*0\r\n"));
    shared.pong = createObject(REDIS_STRING,sdsnew("+PONG\r\n"));
    shared.queued = createObject(REDIS_STRING,sdsnew("+QUEUED\r\n"));
    shared.wrongtypeerr = createObject(REDIS_STRING,sdsnew(
        "-ERR Operation against a key holding the wrong kind of value\r\n"));
    shared.nokeyerr = createObject(REDIS_STRING,sdsnew(
        "-ERR no such key\r\n"));
    shared.syntaxerr = createObject(REDIS_STRING,sdsnew(
        "-ERR syntax error\r\n"));
    shared.sameobjecterr = createObject(REDIS_STRING,sdsnew(
        "-ERR source and destination objects are the same\r\n"));
    shared.outofrangeerr = createObject(REDIS_STRING,sdsnew(
        "-ERR index out of range\r\n"));
    shared.noscripterr = createObject(REDIS_STRING,sdsnew(
        "-NOSCRIPT No matching script. Please use EVAL.\r\n"));
    shared.loadingerr = createObject(REDIS_STRING,sdsnew(
        "-LOADING Redis is loading the dataset in memory\r\n"));
    shared.slowscripterr = createObject(REDIS_STRING,sdsnew(
        "-BUSY Redis is busy running a script. You can only call SCRIPT KILL or SHUTDOWN NOSAVE.\r\n"));
    shared.space = createObject(REDIS_STRING,sdsnew(" "));
    shared.colon = createObject(REDIS_STRING,sdsnew(":"));
    shared.plus = createObject(REDIS_STRING,sdsnew("+"));
    shared.select0 = createStringObject("select 0\r\n",10);
    shared.select1 = createStringObject("select 1\r\n",10);
    shared.select2 = createStringObject("select 2\r\n",10);
    shared.select3 = createStringObject("select 3\r\n",10);
    shared.select4 = createStringObject("select 4\r\n",10);
    shared.select5 = createStringObject("select 5\r\n",10);
    shared.select6 = createStringObject("select 6\r\n",10);
    shared.select7 = createStringObject("select 7\r\n",10);
    shared.select8 = createStringObject("select 8\r\n",10);
    shared.select9 = createStringObject("select 9\r\n",10);
    shared.messagebulk = createStringObject("$7\r\nmessage\r\n",13);
    shared.pmessagebulk = createStringObject("$8\r\npmessage\r\n",14);
    shared.subscribebulk = createStringObject("$9\r\nsubscribe\r\n",15);
    shared.unsubscribebulk = createStringObject("$11\r\nunsubscribe\r\n",18);
    shared.psubscribebulk = createStringObject("$10\r\npsubscribe\r\n",17);
    shared.punsubscribebulk = createStringObject("$12\r\npunsubscribe\r\n",19);
    shared.mbulk3 = createStringObject("*3\r\n",4);
    shared.mbulk4 = createStringObject("*4\r\n",4);
    for (j = 0; j < REDIS_SHARED_INTEGERS; j++) {
        shared.integers[j] = createObject(REDIS_STRING,(void*)(long)j);
        shared.integers[j]->encoding = REDIS_ENCODING_INT;
    }
}

void initServerConfig() {
    server.port = REDIS_SERVERPORT;
    server.bindaddr = NULL;
    server.unixsocket = NULL;
    server.unixsocketperm = 0;
    server.ipfd = -1;
    server.sofd = -1;
    server.dbnum = REDIS_DEFAULT_DBNUM;
    server.verbosity = REDIS_NOTICE;
    server.maxidletime = REDIS_MAXIDLETIME;
    server.client_max_querybuf_len = REDIS_MAX_QUERYBUF_LEN;
    server.saveparams = NULL;
    server.loading = 0;
    server.logfile = NULL; /* NULL = log on standard output */
    server.syslog_enabled = 0;
    server.syslog_ident = zstrdup("redis");
    server.syslog_facility = LOG_LOCAL0;
    server.daemonize = 0;
    server.appendonly = 0;
    server.appendfsync = APPENDFSYNC_EVERYSEC;
    server.no_appendfsync_on_rewrite = 0;
    server.auto_aofrewrite_perc = REDIS_AUTO_AOFREWRITE_PERC;
    server.auto_aofrewrite_min_size = REDIS_AUTO_AOFREWRITE_MIN_SIZE;
    server.auto_aofrewrite_base_size = 0;
    server.aofrewrite_scheduled = 0;
    server.lastfsync = time(NULL);
    server.appendfd = -1;
    server.appendseldb = -1; /* Make sure the first time will not match */
    server.aof_flush_postponed_start = 0;
    server.pidfile = zstrdup("/var/run/redis.pid");
    server.dbfilename = zstrdup("dump.rdb");
    server.appendfilename = zstrdup("appendonly.aof");
    server.requirepass = NULL;
    server.rdbcompression = 1;
    server.activerehashing = 1;
    server.maxclients = REDIS_MAX_CLIENTS;
    server.bpop_blocked_clients = 0;
    server.maxmemory = 0;
    server.maxmemory_policy = REDIS_MAXMEMORY_VOLATILE_LRU;
    server.maxmemory_samples = 3;
    server.hash_max_zipmap_entries = REDIS_HASH_MAX_ZIPMAP_ENTRIES;
    server.hash_max_zipmap_value = REDIS_HASH_MAX_ZIPMAP_VALUE;
    server.list_max_ziplist_entries = REDIS_LIST_MAX_ZIPLIST_ENTRIES;
    server.list_max_ziplist_value = REDIS_LIST_MAX_ZIPLIST_VALUE;
    server.set_max_intset_entries = REDIS_SET_MAX_INTSET_ENTRIES;
    server.zset_max_ziplist_entries = REDIS_ZSET_MAX_ZIPLIST_ENTRIES;
    server.zset_max_ziplist_value = REDIS_ZSET_MAX_ZIPLIST_VALUE;
    server.shutdown_asap = 0;
    server.repl_ping_slave_period = REDIS_REPL_PING_SLAVE_PERIOD;
    server.repl_timeout = REDIS_REPL_TIMEOUT;
    server.cluster_enabled = 0;
    server.cluster.configfile = zstrdup("nodes.conf");
    server.lua_caller = NULL;
    server.lua_time_limit = REDIS_LUA_TIME_LIMIT;
    server.lua_client = NULL;
    server.lua_timedout = 0;

    updateLRUClock();
    resetServerSaveParams();

    appendServerSaveParams(60*60,1);  /* save after 1 hour and 1 change */
    appendServerSaveParams(300,100);  /* save after 5 minutes and 100 changes */
    appendServerSaveParams(60,10000); /* save after 1 minute and 10000 changes */
    /* Replication related */
    server.isslave = 0;
    server.masterauth = NULL;
    server.masterhost = NULL;
    server.masterport = 6379;
    server.master = NULL;
    server.replstate = REDIS_REPL_NONE;
    server.repl_syncio_timeout = REDIS_REPL_SYNCIO_TIMEOUT;
    server.repl_serve_stale_data = 1;
    server.repl_down_since = -1;

    /* Double constants initialization */
    R_Zero = 0.0;
    R_PosInf = 1.0/R_Zero;
    R_NegInf = -1.0/R_Zero;
    R_Nan = R_Zero/R_Zero;

    /* Command table -- we intiialize it here as it is part of the
     * initial configuration, since command names may be changed via
     * redis.conf using the rename-command directive. */
    server.commands = dictCreate(&commandTableDictType,NULL);
    populateCommandTable();
    server.delCommand = lookupCommandByCString("del");
    server.multiCommand = lookupCommandByCString("multi");

    /* Slow log */
    server.slowlog_log_slower_than = REDIS_SLOWLOG_LOG_SLOWER_THAN;
    server.slowlog_max_len = REDIS_SLOWLOG_MAX_LEN;

    /* Assert */
    server.assert_failed = "<no assertion failed>";
    server.assert_file = "<no file>";
    server.assert_line = 0;
    server.bug_report_start = 0;
}

/* This function will try to raise the max number of open files accordingly to
 * the configured max number of clients. It will also account for 32 additional
 * file descriptors as we need a few more for persistence, listening
 * sockets, log files and so forth.
 *
 * If it will not be possible to set the limit accordingly to the configured
 * max number of clients, the function will do the reverse setting
 * server.maxclients to the value that we can actually handle. */
void adjustOpenFilesLimit(void) {
    rlim_t maxfiles = server.maxclients+32;
    struct rlimit limit;

    if (maxfiles < 1024) maxfiles = 1024;
    if (getrlimit(RLIMIT_NOFILE,&limit) == -1) {
        redisLog(REDIS_WARNING,"Unable to obtain the current NOFILE limit (%s), assuming 1024 and setting the max clients configuration accordingly.",
            strerror(errno));
        server.maxclients = 1024-32;
    } else {
        rlim_t oldlimit = limit.rlim_cur;

        /* Set the max number of files if the current limit is not enough
         * for our needs. */
        if (oldlimit < maxfiles) {
            limit.rlim_cur = maxfiles;
            limit.rlim_max = maxfiles;
            if (setrlimit(RLIMIT_NOFILE,&limit) == -1) {
                server.maxclients = oldlimit-32;
                redisLog(REDIS_WARNING,"Unable to set the max number of files limit to %d (%s), setting the max clients configuration to %d.",
                    (int) maxfiles, strerror(errno), (int) server.maxclients);
            } else {
                redisLog(REDIS_NOTICE,"Max number of open files set to %d",
                    (int) maxfiles);
            }
        }
    }
}

void initServer() {
    int j;

    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    setupSignalHandlers();

    if (server.syslog_enabled) {
        openlog(server.syslog_ident, LOG_PID | LOG_NDELAY | LOG_NOWAIT,
            server.syslog_facility);
    }

    server.clients = listCreate();
    server.slaves = listCreate();
    server.monitors = listCreate();
    server.unblocked_clients = listCreate();

    createSharedObjects();
    adjustOpenFilesLimit();
    server.el = aeCreateEventLoop(server.maxclients+1024);
    server.db = zmalloc(sizeof(redisDb)*server.dbnum);

    if (server.port != 0) {
        server.ipfd = anetTcpServer(server.neterr,server.port,server.bindaddr);
        if (server.ipfd == ANET_ERR) {
            redisLog(REDIS_WARNING, "Opening port %d: %s",
                server.port, server.neterr);
            exit(1);
        }
    }
    if (server.unixsocket != NULL) {
        unlink(server.unixsocket); /* don't care if this fails */
        server.sofd = anetUnixServer(server.neterr,server.unixsocket,server.unixsocketperm);
        if (server.sofd == ANET_ERR) {
            redisLog(REDIS_WARNING, "Opening socket: %s", server.neterr);
            exit(1);
        }
    }
    if (server.ipfd < 0 && server.sofd < 0) {
        redisLog(REDIS_WARNING, "Configured to not listen anywhere, exiting.");
        exit(1);
    }
    for (j = 0; j < server.dbnum; j++) {
        server.db[j].dict = dictCreate(&dbDictType,NULL);
        server.db[j].expires = dictCreate(&keyptrDictType,NULL);
        server.db[j].blocking_keys = dictCreate(&keylistDictType,NULL);
        server.db[j].watched_keys = dictCreate(&keylistDictType,NULL);
        server.db[j].id = j;
    }
    server.pubsub_channels = dictCreate(&keylistDictType,NULL);
    server.pubsub_patterns = listCreate();
    listSetFreeMethod(server.pubsub_patterns,freePubsubPattern);
    listSetMatchMethod(server.pubsub_patterns,listMatchPubsubPattern);
    server.cronloops = 0;
    server.bgsavechildpid = -1;
    server.bgrewritechildpid = -1;
    server.bgrewritebuf = sdsempty();
    server.aofbuf = sdsempty();
    server.lastsave = time(NULL);
    server.dirty = 0;
    server.stat_numcommands = 0;
    server.stat_numconnections = 0;
    server.stat_expiredkeys = 0;
    server.stat_evictedkeys = 0;
    server.stat_starttime = time(NULL);
    server.stat_keyspace_misses = 0;
    server.stat_keyspace_hits = 0;
    server.stat_peak_memory = 0;
    server.stat_fork_time = 0;
    server.stat_rejected_conn = 0;
    server.unixtime = time(NULL);
    aeCreateTimeEvent(server.el, 1, serverCron, NULL, NULL);
    if (server.ipfd > 0 && aeCreateFileEvent(server.el,server.ipfd,AE_READABLE,
        acceptTcpHandler,NULL) == AE_ERR) oom("creating file event");
    if (server.sofd > 0 && aeCreateFileEvent(server.el,server.sofd,AE_READABLE,
        acceptUnixHandler,NULL) == AE_ERR) oom("creating file event");

    if (server.appendonly) {
        server.appendfd = open(server.appendfilename,O_WRONLY|O_APPEND|O_CREAT,0644);
        if (server.appendfd == -1) {
            redisLog(REDIS_WARNING, "Can't open the append-only file: %s",
                strerror(errno));
            exit(1);
        }
    }

    if (server.cluster_enabled) clusterInit();
    scriptingInit();
    slowlogInit();
    bioInit();
    srand(time(NULL)^getpid());

}

/* Populates the Redis Command Table starting from the hard coded list
 * we have on top of redis.c file. */
void populateCommandTable(void) {
    int j;
    int numcommands = sizeof(redisCommandTable)/sizeof(struct redisCommand);

    for (j = 0; j < numcommands; j++) {
        struct redisCommand *c = redisCommandTable+j;
        char *f = c->sflags;
        int retval;

        while(*f != '\0') {
            switch(*f) {
            case 'w': c->flags |= REDIS_CMD_WRITE; break;
            case 'r': c->flags |= REDIS_CMD_READONLY; break;
            case 'm': c->flags |= REDIS_CMD_DENYOOM; break;
            case 'a': c->flags |= REDIS_CMD_ADMIN; break;
            case 'p': c->flags |= REDIS_CMD_PUBSUB; break;
            case 'f': c->flags |= REDIS_CMD_FORCE_REPLICATION; break;
            case 's': c->flags |= REDIS_CMD_NOSCRIPT; break;
            case 'R': c->flags |= REDIS_CMD_RANDOM; break;
            default: redisPanic("Unsupported command flag"); break;
            }
            f++;
        }

        retval = dictAdd(server.commands, sdsnew(c->name), c);
        assert(retval == DICT_OK);
    }
}

void resetCommandTableStats(void) {
    int numcommands = sizeof(redisCommandTable)/sizeof(struct redisCommand);
    int j;

    for (j = 0; j < numcommands; j++) {
        struct redisCommand *c = redisCommandTable+j;

        c->microseconds = 0;
        c->calls = 0;
    }
}

/* ====================== Commands lookup and execution ===================== */

struct redisCommand *lookupCommand(sds name) {
    return dictFetchValue(server.commands, name);
}

struct redisCommand *lookupCommandByCString(char *s) {
    struct redisCommand *cmd;
    sds name = sdsnew(s);

    cmd = dictFetchValue(server.commands, name);
    sdsfree(name);
    return cmd;
}

/* Call() is the core of Redis execution of a command */
void call(redisClient *c) {
    long long dirty, start = ustime(), duration;

    dirty = server.dirty;
    c->cmd->proc(c);
    dirty = server.dirty-dirty;
    duration = ustime()-start;
    c->cmd->microseconds += duration;
    slowlogPushEntryIfNeeded(c->argv,c->argc,duration);
    c->cmd->calls++;

    if (server.appendonly && dirty > 0)
        feedAppendOnlyFile(c->cmd,c->db->id,c->argv,c->argc);
    if ((dirty > 0 || c->cmd->flags & REDIS_CMD_FORCE_REPLICATION) &&
        listLength(server.slaves))
        replicationFeedSlaves(server.slaves,c->db->id,c->argv,c->argc);
    if (listLength(server.monitors))
        replicationFeedMonitors(server.monitors,c->db->id,c->argv,c->argc);
    server.stat_numcommands++;
}

static void redislite_add_reply(redisClient *c, redislite_reply *reply) {
	switch (reply->type) {
		case REDISLITE_REPLY_NIL:
			addReply(c, shared.nullbulk);
			break;

		case REDISLITE_REPLY_STRING:
			addReplyBulkCBuffer(c, reply->str, reply->len);
			break;

		case REDISLITE_REPLY_INTEGER:
			addReplyLongLong(c, reply->integer);
			break;

		case REDISLITE_REPLY_STATUS:
			{
				if (reply->str[reply->len - 1] != 0) reply->str[reply->len] = 0;
				addReplyStatus(c, reply->str);
				break;
			}
		case REDISLITE_REPLY_ERROR:
			{
				if (reply->str[reply->len - 1] != 0) reply->str[reply->len] = 0;
				addReplyError(c, reply->str);
				break;
			}
		case REDISLITE_REPLY_ARRAY:
			{
				addReplyMultiBulkLen(c, reply->elements);
				size_t i;
				for (i = 0; i < reply->elements; i++) {
					redislite_add_reply(c, reply->element[i]);
				}
				break;
			}
		default:
			{
				addReplyError(c, "Unknown reply");
				break;
			}
	}
}

/* If this function gets called we already read a whole
 * command, argments are in the client argv/argc fields.
 * processCommand() execute the command or prepare the
 * server for a bulk read from the client.
 *
 * If 1 is returned the client is still alive and valid and
 * and other operations can be performed by the caller. Otherwise
 * if 0 is returned the client was destroied (i.e. after QUIT). */
int processCommand(redisClient *c) {
    /* The QUIT command is handled separately. Normal command procs will
     * go through checking for replication and QUIT will cause trouble
     * when FORCE_REPLICATION is enabled and would be implemented in
     * a regular command proc. */
    if (!strcasecmp(c->argv[0]->ptr,"quit")) {
        addReply(c,shared.ok);
        c->flags |= REDIS_CLOSE_AFTER_REPLY;
        return REDIS_ERR;
    }

    /* Now lookup the command and check ASAP about trivial error conditions
     * such as wrong arity, bad command name and so forth. */
    c->cmd = c->lastcmd = lookupCommand(c->argv[0]->ptr);
    if (!c->cmd) {
        addReplyErrorFormat(c,"unknown command '%s'",
            (char*)c->argv[0]->ptr);
        return REDIS_OK;
    } else if ((c->cmd->arity > 0 && c->cmd->arity != c->argc) ||
               (c->argc < -c->cmd->arity)) {
        addReplyErrorFormat(c,"wrong number of arguments for '%s' command",
            c->cmd->name);
        return REDIS_OK;
    }

    /* If cluster is enabled, redirect here */
    if (server.cluster_enabled &&
                !(c->cmd->getkeys_proc == NULL && c->cmd->firstkey == 0)) {
        int hashslot;

        if (server.cluster.state != REDIS_CLUSTER_OK) {
            addReplyError(c,"The cluster is down. Check with CLUSTER INFO for more information");
            return REDIS_OK;
        } else {
            int ask;
            clusterNode *n = getNodeByQuery(c,c->cmd,c->argv,c->argc,&hashslot,&ask);
            if (n == NULL) {
                addReplyError(c,"Multi keys request invalid in cluster");
                return REDIS_OK;
            } else if (n != server.cluster.myself) {
                addReplySds(c,sdscatprintf(sdsempty(),
                    "-%s %d %s:%d\r\n", ask ? "ASK" : "MOVED",
                    hashslot,n->ip,n->port));
                return REDIS_OK;
            }
        }
    }

    /* Handle the maxmemory directive.
     *
     * First we try to free some memory if possible (if there are volatile
     * keys in the dataset). If there are not the only thing we can do
     * is returning an error. */
    if (server.maxmemory) freeMemoryIfNeeded();
    if (server.maxmemory && (c->cmd->flags & REDIS_CMD_DENYOOM) &&
        zmalloc_used_memory() > server.maxmemory)
    {
        addReplyError(c,"command not allowed when used memory > 'maxmemory'");
        return REDIS_OK;
    }

    /* Only allow SUBSCRIBE and UNSUBSCRIBE in the context of Pub/Sub */
    if ((dictSize(c->pubsub_channels) > 0 || listLength(c->pubsub_patterns) > 0)
        &&
        c->cmd->proc != subscribeCommand &&
        c->cmd->proc != unsubscribeCommand &&
        c->cmd->proc != psubscribeCommand &&
        c->cmd->proc != punsubscribeCommand) {
        addReplyError(c,"only (P)SUBSCRIBE / (P)UNSUBSCRIBE / QUIT allowed in this context");
        return REDIS_OK;
    }

    /* Only allow INFO and SLAVEOF when slave-serve-stale-data is no and
     * we are a slave with a broken link with master. */
    if (server.masterhost && server.replstate != REDIS_REPL_CONNECTED &&
        server.repl_serve_stale_data == 0 &&
        c->cmd->proc != infoCommand && c->cmd->proc != slaveofCommand)
    {
        addReplyError(c,
            "link with MASTER is down and slave-serve-stale-data is set to no");
        return REDIS_OK;
    }

    /* Loading DB? Return an error if the command is not INFO */
    if (server.loading && c->cmd->proc != infoCommand) {
        addReply(c, shared.loadingerr);
        return REDIS_OK;
    }

    /* Lua script too slow? Only allow SHUTDOWN NOSAVE and SCRIPT KILL. */
    if (server.lua_timedout &&
        !(c->cmd->proc != shutdownCommand &&
          c->argc == 2 &&
          tolower(((char*)c->argv[1]->ptr)[0]) == 'n') &&
        !(c->cmd->proc == scriptCommand &&
          c->argc == 2 &&
          tolower(((char*)c->argv[1]->ptr)[0]) == 'k'))
    {
        addReply(c, shared.slowscripterr);
        return REDIS_OK;
    }

    /* Exec the command */
    if (c->flags & REDIS_MULTI &&
        c->cmd->proc != execCommand && c->cmd->proc != discardCommand &&
        c->cmd->proc != multiCommand && c->cmd->proc != watchCommand)
    {
        queueMultiCommand(c);
        addReply(c,shared.queued);
    } else {
		if (sdslen(c->argv[0]->ptr) == 4) {
			sds command = sdsnewlen(command, 4);
			sdstoupper(command);
			if (strcmp(command, "AUTH") == 0) {
				authCommand(c);
				sdsfree(command);
				return REDIS_OK;
			}
			sdsfree(command);
		}
		char **argv = redislite_malloc(sizeof(char*) * c->argc);
		size_t *argvlen = redislite_malloc(sizeof(size_t) * c->argc);
		int i;
		for (i = 0; i < c->argc; i++) {
			argv[i] = c->argv[i]->ptr;
			argvlen[i] = sdslen(c->argv[i]->ptr);
		}
		redislite *db = redislite_open_database("redislite-server.db");
		redislite_reply *reply = redislite_command_argv(db, c->argc, (const char**)argv, (const size_t*)argvlen);
		redislite_add_reply(c, reply);
		redislite_free_reply(reply);
		redislite_free(argv);
		redislite_free(argvlen);
		redislite_close_database(db);
    }
    return REDIS_OK;
}

/*================================== Shutdown =============================== */

int prepareForShutdown(int flags) {
    int save = flags & REDIS_SHUTDOWN_SAVE;
    int nosave = flags & REDIS_SHUTDOWN_NOSAVE;

    redisLog(REDIS_WARNING,"User requested shutdown...");
    /* Kill the saving child if there is a background saving in progress.
       We want to avoid race conditions, for instance our saving child may
       overwrite the synchronous saving did by SHUTDOWN. */
    if (server.bgsavechildpid != -1) {
        redisLog(REDIS_WARNING,"There is a child saving an .rdb. Killing it!");
        kill(server.bgsavechildpid,SIGKILL);
        rdbRemoveTempFile(server.bgsavechildpid);
    }
    if (server.appendonly) {
        /* Kill the AOF saving child as the AOF we already have may be longer
         * but contains the full dataset anyway. */
        if (server.bgrewritechildpid != -1) {
            redisLog(REDIS_WARNING,
                "There is a child rewriting the AOF. Killing it!");
            kill(server.bgrewritechildpid,SIGKILL);
        }
        /* Append only file: fsync() the AOF and exit */
        redisLog(REDIS_NOTICE,"Calling fsync() on the AOF file.");
        aof_fsync(server.appendfd);
    }
    if ((server.saveparamslen > 0 && !nosave) || save) {
        redisLog(REDIS_NOTICE,"Saving the final RDB snapshot before exiting.");
        /* Snapshotting. Perform a SYNC SAVE and exit */
        if (rdbSave(server.dbfilename) != REDIS_OK) {
            /* Ooops.. error saving! The best we can do is to continue
             * operating. Note that if there was a background saving process,
             * in the next cron() Redis will be notified that the background
             * saving aborted, handling special stuff like slaves pending for
             * synchronization... */
            redisLog(REDIS_WARNING,"Error trying to save the DB, can't exit.");
            return REDIS_ERR;
        }
    }
    if (server.daemonize) {
        redisLog(REDIS_NOTICE,"Removing the pid file.");
        unlink(server.pidfile);
    }
    /* Close the listening sockets. Apparently this allows faster restarts. */
    if (server.ipfd != -1) close(server.ipfd);
    if (server.sofd != -1) close(server.sofd);
    if (server.unixsocket) {
        redisLog(REDIS_NOTICE,"Removing the unix socket file.");
        unlink(server.unixsocket); /* don't care if this fails */
    }

    redisLog(REDIS_WARNING,"Redis is now ready to exit, bye bye...");
    return REDIS_OK;
}

/*================================== Commands =============================== */

void authCommand(redisClient *c) {
    if (!server.requirepass) {
        addReplyError(c,"Client sent AUTH, but no password is set");
    } else if (!strcmp(c->argv[1]->ptr, server.requirepass)) {
      c->authenticated = 1;
      addReply(c,shared.ok);
    } else {
      c->authenticated = 0;
      addReplyError(c,"invalid password");
    }
}

void pingCommand(redisClient *c) {
    addReply(c,shared.pong);
}

void echoCommand(redisClient *c) {
    addReplyBulk(c,c->argv[1]);
}

/* Convert an amount of bytes into a human readable string in the form
 * of 100B, 2G, 100M, 4K, and so forth. */
void bytesToHuman(char *s, unsigned long long n) {
    double d;

    if (n < 1024) {
        /* Bytes */
        sprintf(s,"%lluB",n);
        return;
    } else if (n < (1024*1024)) {
        d = (double)n/(1024);
        sprintf(s,"%.2fK",d);
    } else if (n < (1024LL*1024*1024)) {
        d = (double)n/(1024*1024);
        sprintf(s,"%.2fM",d);
    } else if (n < (1024LL*1024*1024*1024)) {
        d = (double)n/(1024LL*1024*1024);
        sprintf(s,"%.2fG",d);
    }
}

/* Create the string returned by the INFO command. This is decoupled
 * by the INFO command itself as we need to report the same information
 * on memory corruption problems. */
sds genRedisInfoString(char *section) {
    sds info = sdsempty();
    time_t uptime = time(NULL)-server.stat_starttime;
    int j, numcommands;
    struct rusage self_ru, c_ru;
    unsigned long lol, bib;
    int allsections = 0, defsections = 0;
    int sections = 0;

    if (section) {
        allsections = strcasecmp(section,"all") == 0;
        defsections = strcasecmp(section,"default") == 0;
    }

    getrusage(RUSAGE_SELF, &self_ru);
    getrusage(RUSAGE_CHILDREN, &c_ru);
    getClientsMaxBuffers(&lol,&bib);

    /* Server */
    if (allsections || defsections || !strcasecmp(section,"server")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Server\r\n"
            "redis_version:%s\r\n"
            "redis_git_sha1:%s\r\n"
            "redis_git_dirty:%d\r\n"
            "arch_bits:%s\r\n"
            "multiplexing_api:%s\r\n"
            "process_id:%ld\r\n"
            "tcp_port:%d\r\n"
            "uptime_in_seconds:%ld\r\n"
            "uptime_in_days:%ld\r\n"
            "lru_clock:%ld\r\n",
            REDIS_VERSION,
            redisGitSHA1(),
            strtol(redisGitDirty(),NULL,10) > 0,
            (sizeof(long) == 8) ? "64" : "32",
            aeGetApiName(),
            (long) getpid(),
            server.port,
            uptime,
            uptime/(3600*24),
            (unsigned long) server.lruclock);
    }

    /* Clients */
    if (allsections || defsections || !strcasecmp(section,"clients")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Clients\r\n"
            "connected_clients:%d\r\n"
            "client_longest_output_list:%lu\r\n"
            "client_biggest_input_buf:%lu\r\n"
            "blocked_clients:%d\r\n",
            listLength(server.clients)-listLength(server.slaves),
            lol, bib,
            server.bpop_blocked_clients);
    }

    /* Memory */
    if (allsections || defsections || !strcasecmp(section,"memory")) {
        char hmem[64];
        char peak_hmem[64];

        bytesToHuman(hmem,zmalloc_used_memory());
        bytesToHuman(peak_hmem,server.stat_peak_memory);
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Memory\r\n"
            "used_memory:%zu\r\n"
            "used_memory_human:%s\r\n"
            "used_memory_rss:%zu\r\n"
            "used_memory_peak:%zu\r\n"
            "used_memory_peak_human:%s\r\n"
            "used_memory_lua:%lld\r\n"
            "mem_fragmentation_ratio:%.2f\r\n"
            "mem_allocator:%s\r\n",
            zmalloc_used_memory(),
            hmem,
            zmalloc_get_rss(),
            server.stat_peak_memory,
            peak_hmem,
            ((long long)lua_gc(server.lua,LUA_GCCOUNT,0))*1024LL,
            zmalloc_get_fragmentation_ratio(),
            ZMALLOC_LIB
            );
    }

    /* Persistence */
    if (allsections || defsections || !strcasecmp(section,"persistence")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Persistence\r\n"
            "loading:%d\r\n"
            "aof_enabled:%d\r\n"
            "changes_since_last_save:%lld\r\n"
            "bgsave_in_progress:%d\r\n"
            "last_save_time:%ld\r\n"
            "bgrewriteaof_in_progress:%d\r\n",
            server.loading,
            server.appendonly,
            server.dirty,
            server.bgsavechildpid != -1,
            server.lastsave,
            server.bgrewritechildpid != -1);

        if (server.appendonly) {
            info = sdscatprintf(info,
                "aof_current_size:%lld\r\n"
                "aof_base_size:%lld\r\n"
                "aof_pending_rewrite:%d\r\n"
                "aof_buffer_length:%zu\r\n"
                "aof_pending_bio_fsync:%llu\r\n",
                (long long) server.appendonly_current_size,
                (long long) server.auto_aofrewrite_base_size,
                server.aofrewrite_scheduled,
                sdslen(server.aofbuf),
                bioPendingJobsOfType(REDIS_BIO_AOF_FSYNC));
        }

        if (server.loading) {
            double perc;
            time_t eta, elapsed;
            off_t remaining_bytes = server.loading_total_bytes-
                                    server.loading_loaded_bytes;

            perc = ((double)server.loading_loaded_bytes /
                   server.loading_total_bytes) * 100;

            elapsed = time(NULL)-server.loading_start_time;
            if (elapsed == 0) {
                eta = 1; /* A fake 1 second figure if we don't have
                            enough info */
            } else {
                eta = (elapsed*remaining_bytes)/server.loading_loaded_bytes;
            }

            info = sdscatprintf(info,
                "loading_start_time:%ld\r\n"
                "loading_total_bytes:%llu\r\n"
                "loading_loaded_bytes:%llu\r\n"
                "loading_loaded_perc:%.2f\r\n"
                "loading_eta_seconds:%ld\r\n"
                ,(unsigned long) server.loading_start_time,
                (unsigned long long) server.loading_total_bytes,
                (unsigned long long) server.loading_loaded_bytes,
                perc,
                eta
            );
        }
    }

    /* Stats */
    if (allsections || defsections || !strcasecmp(section,"stats")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Stats\r\n"
            "total_connections_received:%lld\r\n"
            "total_commands_processed:%lld\r\n"
            "rejected_connections:%lld\r\n"
            "expired_keys:%lld\r\n"
            "evicted_keys:%lld\r\n"
            "keyspace_hits:%lld\r\n"
            "keyspace_misses:%lld\r\n"
            "pubsub_channels:%ld\r\n"
            "pubsub_patterns:%u\r\n"
            "latest_fork_usec:%lld\r\n",
            server.stat_numconnections,
            server.stat_numcommands,
            server.stat_rejected_conn,
            server.stat_expiredkeys,
            server.stat_evictedkeys,
            server.stat_keyspace_hits,
            server.stat_keyspace_misses,
            dictSize(server.pubsub_channels),
            listLength(server.pubsub_patterns),
            server.stat_fork_time);
    }

    /* Replication */
    if (allsections || defsections || !strcasecmp(section,"replication")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
            "# Replication\r\n"
            "role:%s\r\n",
            server.masterhost == NULL ? "master" : "slave");
        if (server.masterhost) {
            info = sdscatprintf(info,
                "master_host:%s\r\n"
                "master_port:%d\r\n"
                "master_link_status:%s\r\n"
                "master_last_io_seconds_ago:%d\r\n"
                "master_sync_in_progress:%d\r\n"
                ,server.masterhost,
                server.masterport,
                (server.replstate == REDIS_REPL_CONNECTED) ?
                    "up" : "down",
                server.master ?
                ((int)(time(NULL)-server.master->lastinteraction)) : -1,
                server.replstate == REDIS_REPL_TRANSFER
            );

            if (server.replstate == REDIS_REPL_TRANSFER) {
                info = sdscatprintf(info,
                    "master_sync_left_bytes:%ld\r\n"
                    "master_sync_last_io_seconds_ago:%d\r\n"
                    ,(long)server.repl_transfer_left,
                    (int)(time(NULL)-server.repl_transfer_lastio)
                );
            }

            if (server.replstate != REDIS_REPL_CONNECTED) {
                info = sdscatprintf(info,
                    "master_link_down_since_seconds:%ld\r\n",
                    (long)time(NULL)-server.repl_down_since);
            }
        }
        info = sdscatprintf(info,
            "connected_slaves:%d\r\n",
            listLength(server.slaves));
        if (listLength(server.slaves)) {
            int slaveid = 0;
            listNode *ln;
            listIter li;

            listRewind(server.slaves,&li);
            while((ln = listNext(&li))) {
                redisClient *slave = listNodeValue(ln);
                char *state = NULL;
                char ip[32];
                int port;

                if (anetPeerToString(slave->fd,ip,&port) == -1) continue;
                switch(slave->replstate) {
                case REDIS_REPL_WAIT_BGSAVE_START:
                case REDIS_REPL_WAIT_BGSAVE_END:
                    state = "wait_bgsave";
                    break;
                case REDIS_REPL_SEND_BULK:
                    state = "send_bulk";
                    break;
                case REDIS_REPL_ONLINE:
                    state = "online";
                    break;
                }
                if (state == NULL) continue;
                info = sdscatprintf(info,"slave%d:%s,%d,%s\r\n",
                    slaveid,ip,port,state);
                slaveid++;
            }
        }
    }

    /* CPU */
    if (allsections || defsections || !strcasecmp(section,"cpu")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
        "# CPU\r\n"
        "used_cpu_sys:%.2f\r\n"
        "used_cpu_user:%.2f\r\n"
        "used_cpu_sys_children:%.2f\r\n"
        "used_cpu_user_children:%.2f\r\n",
        (float)self_ru.ru_stime.tv_sec+(float)self_ru.ru_stime.tv_usec/1000000,
        (float)self_ru.ru_utime.tv_sec+(float)self_ru.ru_utime.tv_usec/1000000,
        (float)c_ru.ru_stime.tv_sec+(float)c_ru.ru_stime.tv_usec/1000000,
        (float)c_ru.ru_utime.tv_sec+(float)c_ru.ru_utime.tv_usec/1000000);
    }

    /* cmdtime */
    if (allsections || !strcasecmp(section,"commandstats")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info, "# Commandstats\r\n");
        numcommands = sizeof(redisCommandTable)/sizeof(struct redisCommand);
        for (j = 0; j < numcommands; j++) {
            struct redisCommand *c = redisCommandTable+j;

            if (!c->calls) continue;
            info = sdscatprintf(info,
                "cmdstat_%s:calls=%lld,usec=%lld,usec_per_call=%.2f\r\n",
                c->name, c->calls, c->microseconds,
                (c->calls == 0) ? 0 : ((float)c->microseconds/c->calls));
        }
    }

    /* Clusetr */
    if (allsections || defsections || !strcasecmp(section,"cluster")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info,
        "# Cluster\r\n"
        "cluster_enabled:%d\r\n",
        server.cluster_enabled);
    }

    /* Key space */
    if (allsections || defsections || !strcasecmp(section,"keyspace")) {
        if (sections++) info = sdscat(info,"\r\n");
        info = sdscatprintf(info, "# Keyspace\r\n");
        for (j = 0; j < server.dbnum; j++) {
            long long keys, vkeys;

            keys = dictSize(server.db[j].dict);
            vkeys = dictSize(server.db[j].expires);
            if (keys || vkeys) {
                info = sdscatprintf(info, "db%d:keys=%lld,expires=%lld\r\n",
                    j, keys, vkeys);
            }
        }
    }
    return info;
}

void infoCommand(redisClient *c) {
    char *section = c->argc == 2 ? c->argv[1]->ptr : "default";

    if (c->argc > 2) {
        addReply(c,shared.syntaxerr);
        return;
    }
    sds info = genRedisInfoString(section);
    addReplySds(c,sdscatprintf(sdsempty(),"$%lu\r\n",
        (unsigned long)sdslen(info)));
    addReplySds(c,info);
    addReply(c,shared.crlf);
}

void monitorCommand(redisClient *c) {
    /* ignore MONITOR if aleady slave or in monitor mode */
    if (c->flags & REDIS_SLAVE) return;

    c->flags |= (REDIS_SLAVE|REDIS_MONITOR);
    c->slaveseldb = 0;
    listAddNodeTail(server.monitors,c);
    addReply(c,shared.ok);
}

/* ============================ Maxmemory directive  ======================== */

/* This function gets called when 'maxmemory' is set on the config file to limit
 * the max memory used by the server, and we are out of memory.
 * This function will try to, in order:
 *
 * - Free objects from the free list
 * - Try to remove keys with an EXPIRE set
 *
 * It is not possible to free enough memory to reach used-memory < maxmemory
 * the server will start refusing commands that will enlarge even more the
 * memory usage.
 */
void freeMemoryIfNeeded(void) {
    /* Remove keys accordingly to the active policy as long as we are
     * over the memory limit. */
    if (server.maxmemory_policy == REDIS_MAXMEMORY_NO_EVICTION) return;

    while (server.maxmemory && zmalloc_used_memory() > server.maxmemory) {
        int j, k, freed = 0;

        for (j = 0; j < server.dbnum; j++) {
            long bestval = 0; /* just to prevent warning */
            sds bestkey = NULL;
            struct dictEntry *de;
            redisDb *db = server.db+j;
            dict *dict;

            if (server.maxmemory_policy == REDIS_MAXMEMORY_ALLKEYS_LRU ||
                server.maxmemory_policy == REDIS_MAXMEMORY_ALLKEYS_RANDOM)
            {
                dict = server.db[j].dict;
            } else {
                dict = server.db[j].expires;
            }
            if (dictSize(dict) == 0) continue;

            /* volatile-random and allkeys-random policy */
            if (server.maxmemory_policy == REDIS_MAXMEMORY_ALLKEYS_RANDOM ||
                server.maxmemory_policy == REDIS_MAXMEMORY_VOLATILE_RANDOM)
            {
                de = dictGetRandomKey(dict);
                bestkey = dictGetKey(de);
            }

            /* volatile-lru and allkeys-lru policy */
            else if (server.maxmemory_policy == REDIS_MAXMEMORY_ALLKEYS_LRU ||
                server.maxmemory_policy == REDIS_MAXMEMORY_VOLATILE_LRU)
            {
                for (k = 0; k < server.maxmemory_samples; k++) {
                    sds thiskey;
                    long thisval;
                    robj *o;

                    de = dictGetRandomKey(dict);
                    thiskey = dictGetKey(de);
                    /* When policy is volatile-lru we need an additonal lookup
                     * to locate the real key, as dict is set to db->expires. */
                    if (server.maxmemory_policy == REDIS_MAXMEMORY_VOLATILE_LRU)
                        de = dictFind(db->dict, thiskey);
                    o = dictGetVal(de);
                    thisval = estimateObjectIdleTime(o);

                    /* Higher idle time is better candidate for deletion */
                    if (bestkey == NULL || thisval > bestval) {
                        bestkey = thiskey;
                        bestval = thisval;
                    }
                }
            }

            /* volatile-ttl */
            else if (server.maxmemory_policy == REDIS_MAXMEMORY_VOLATILE_TTL) {
                for (k = 0; k < server.maxmemory_samples; k++) {
                    sds thiskey;
                    long thisval;

                    de = dictGetRandomKey(dict);
                    thiskey = dictGetKey(de);
                    thisval = (long) dictGetVal(de);

                    /* Expire sooner (minor expire unix timestamp) is better
                     * candidate for deletion */
                    if (bestkey == NULL || thisval < bestval) {
                        bestkey = thiskey;
                        bestval = thisval;
                    }
                }
            }

            /* Finally remove the selected key. */
            if (bestkey) {
                robj *keyobj = createStringObject(bestkey,sdslen(bestkey));
                propagateExpire(db,keyobj);
                dbDelete(db,keyobj);
                server.stat_evictedkeys++;
                decrRefCount(keyobj);
                freed++;
            }
        }
        if (!freed) return; /* nothing to free... */
    }
}

/* =================================== Main! ================================ */

#ifdef __linux__
int linuxOvercommitMemoryValue(void) {
    FILE *fp = fopen("/proc/sys/vm/overcommit_memory","r");
    char buf[64];

    if (!fp) return -1;
    if (fgets(buf,64,fp) == NULL) {
        fclose(fp);
        return -1;
    }
    fclose(fp);

    return atoi(buf);
}

void linuxOvercommitMemoryWarning(void) {
    if (linuxOvercommitMemoryValue() == 0) {
        redisLog(REDIS_WARNING,"WARNING overcommit_memory is set to 0! Background save may fail under low memory condition. To fix this issue add 'vm.overcommit_memory = 1' to /etc/sysctl.conf and then reboot or run the command 'sysctl vm.overcommit_memory=1' for this to take effect.");
    }
}
#endif /* __linux__ */

void createPidFile(void) {
    /* Try to write the pid file in a best-effort way. */
    FILE *fp = fopen(server.pidfile,"w");
    if (fp) {
        fprintf(fp,"%d\n",(int)getpid());
        fclose(fp);
    }
}

void daemonize(void) {
    int fd;

    if (fork() != 0) exit(0); /* parent exits */
    setsid(); /* create a new session */

    /* Every output goes to /dev/null. If Redis is daemonized but
     * the 'logfile' is set to 'stdout' in the configuration file
     * it will not log at all. */
    if ((fd = open("/dev/null", O_RDWR, 0)) != -1) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO) close(fd);
    }
}

void version() {
    printf("Redis server version %s (%s:%d)\n", REDIS_VERSION,
        redisGitSHA1(), atoi(redisGitDirty()) > 0);
    exit(0);
}

void usage() {
    fprintf(stderr,"Usage: ./redis-server [/path/to/redis.conf] [options]\n");
    fprintf(stderr,"       ./redis-server - (read config from stdin)\n");
    fprintf(stderr,"       ./redis-server -v or --version\n");
    fprintf(stderr,"       ./redis-server -h or --help\n\n");
    fprintf(stderr,"Examples:\n");
    fprintf(stderr,"       ./redis-server (run the server with default conf)\n");
    fprintf(stderr,"       ./redis-server /etc/redis/6379.conf\n");
    fprintf(stderr,"       ./redis-server --port 7777\n");
    fprintf(stderr,"       ./redis-server --port 7777 --slaveof 127.0.0.1 8888\n");
    fprintf(stderr,"       ./redis-server /etc/myredis.conf --loglevel verbose\n");
    exit(1);
}

void redisAsciiArt(void) {
#include "asciilogo.h"
    char *buf = zmalloc(1024*16);

    snprintf(buf,1024*16,ascii_logo,
        REDIS_VERSION,
        redisGitSHA1(),
        strtol(redisGitDirty(),NULL,10) > 0,
        (sizeof(long) == 8) ? "64" : "32",
        server.cluster_enabled ? "cluster" : "stand alone",
        server.port,
        (long) getpid()
    );
    redisLogRaw(REDIS_NOTICE|REDIS_LOG_RAW,buf);
    zfree(buf);
}

#ifdef HAVE_BACKTRACE
static void *getMcontextEip(ucontext_t *uc) {
#if defined(__FreeBSD__)
    return (void*) uc->uc_mcontext.mc_eip;
#elif defined(__dietlibc__)
    return (void*) uc->uc_mcontext.eip;
#elif defined(__APPLE__) && !defined(MAC_OS_X_VERSION_10_6)
  #if __x86_64__
    return (void*) uc->uc_mcontext->__ss.__rip;
  #elif __i386__
    return (void*) uc->uc_mcontext->__ss.__eip;
  #else
    return (void*) uc->uc_mcontext->__ss.__srr0;
  #endif
#elif defined(__APPLE__) && defined(MAC_OS_X_VERSION_10_6)
  #if defined(_STRUCT_X86_THREAD_STATE64) && !defined(__i386__)
    return (void*) uc->uc_mcontext->__ss.__rip;
  #else
    return (void*) uc->uc_mcontext->__ss.__eip;
  #endif
#elif defined(__i386__)
    return (void*) uc->uc_mcontext.gregs[14]; /* Linux 32 */
#elif defined(__X86_64__) || defined(__x86_64__)
    return (void*) uc->uc_mcontext.gregs[16]; /* Linux 64 */
#elif defined(__ia64__) /* Linux IA64 */
    return (void*) uc->uc_mcontext.sc_ip;
#else
    return NULL;
#endif
}

void bugReportStart(void) {
    if (server.bug_report_start == 0) {
        redisLog(REDIS_WARNING,
            "=== REDIS BUG REPORT START: Cut & paste starting from here ===");
        server.bug_report_start = 1;
    }
}

static void sigsegvHandler(int sig, siginfo_t *info, void *secret) {
    void *trace[100];
    char **messages = NULL;
    int i, trace_size = 0;
    ucontext_t *uc = (ucontext_t*) secret;
    sds infostring, clients;
    struct sigaction act;
    REDIS_NOTUSED(info);

    bugReportStart();
    redisLog(REDIS_WARNING,
        "    Redis %s crashed by signal: %d", REDIS_VERSION, sig);
    redisLog(REDIS_WARNING,
        "    Failed assertion: %s (%s:%d)", server.assert_failed,
                        server.assert_file, server.assert_line);

    /* Generate the stack trace */
    trace_size = backtrace(trace, 100);

    /* overwrite sigaction with caller's address */
    if (getMcontextEip(uc) != NULL) {
        trace[1] = getMcontextEip(uc);
    }
    messages = backtrace_symbols(trace, trace_size);
    redisLog(REDIS_WARNING, "--- STACK TRACE");
    for (i=1; i<trace_size; ++i)
        redisLog(REDIS_WARNING,"%s", messages[i]);

    /* Log INFO and CLIENT LIST */
    redisLog(REDIS_WARNING, "--- INFO OUTPUT");
    infostring = genRedisInfoString("all");
    redisLogRaw(REDIS_WARNING, infostring);
    redisLog(REDIS_WARNING, "--- CLIENT LIST OUTPUT");
    clients = getAllClientsInfoString();
    redisLogRaw(REDIS_WARNING, clients);
    /* Don't sdsfree() strings to avoid a crash. Memory may be corrupted. */

    redisLog(REDIS_WARNING,
"=== REDIS BUG REPORT END. Make sure to include from START to END. ===\n\n"
"    Please report the crash opening an issue on github:\n\n"
"        http://github.com/antirez/redis/issues\n\n"
);
    /* free(messages); Don't call free() with possibly corrupted memory. */
    if (server.daemonize) unlink(server.pidfile);

    /* Make sure we exit with the right signal at the end. So for instance
     * the core will be dumped if enabled. */
    sigemptyset (&act.sa_mask);
    /* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction
     * is used. Otherwise, sa_handler is used */
    act.sa_flags = SA_NODEFER | SA_ONSTACK | SA_RESETHAND;
    act.sa_handler = SIG_DFL;
    sigaction (sig, &act, NULL);
    kill(getpid(),sig);
}
#endif /* HAVE_BACKTRACE */

static void sigtermHandler(int sig) {
    REDIS_NOTUSED(sig);

    redisLog(REDIS_WARNING,"Received SIGTERM, scheduling shutdown...");
    server.shutdown_asap = 1;
}

void setupSignalHandlers(void) {
    struct sigaction act;

    /* When the SA_SIGINFO flag is set in sa_flags then sa_sigaction is used.
     * Otherwise, sa_handler is used. */
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_ONSTACK | SA_RESETHAND;
    act.sa_handler = sigtermHandler;
    sigaction(SIGTERM, &act, NULL);

#ifdef HAVE_BACKTRACE
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_NODEFER | SA_ONSTACK | SA_RESETHAND | SA_SIGINFO;
    act.sa_sigaction = sigsegvHandler;
    sigaction(SIGSEGV, &act, NULL);
    sigaction(SIGBUS, &act, NULL);
    sigaction(SIGFPE, &act, NULL);
    sigaction(SIGILL, &act, NULL);
#endif
    return;
}

int main(int argc, char **argv) {
    long long start;

    zmalloc_enable_thread_safeness();
    initServerConfig();
    if (argc >= 2) {
        int j = 1; /* First option to parse in argv[] */
        sds options = sdsempty();
        char *configfile = NULL;

        /* Handle special options --help and --version */
        if (strcmp(argv[1], "-v") == 0 ||
            strcmp(argv[1], "--version") == 0) version();
        if (strcmp(argv[1], "--help") == 0 ||
            strcmp(argv[1], "-h") == 0) usage();
        /* First argument is the config file name? */
        if (argv[j][0] != '-' || argv[j][1] != '-')
            configfile = argv[j++];
        /* All the other options are parsed and conceptually appended to the
         * configuration file. For instance --port 6380 will generate the
         * string "port 6380\n" to be parsed after the actual file name
         * is parsed, if any. */
        while(j != argc) {
            if (argv[j][0] == '-' && argv[j][1] == '-') {
                /* Option name */
                if (sdslen(options)) options = sdscat(options,"\n");
                options = sdscat(options,argv[j]+2);
                options = sdscat(options," ");
            } else {
                /* Option argument */
                options = sdscatrepr(options,argv[j],strlen(argv[j]));
                options = sdscat(options," ");
            }
            j++;
        }
        resetServerSaveParams();
        loadServerConfig(configfile,options);
        sdsfree(options);
    } else {
        redisLog(REDIS_WARNING,"Warning: no config file specified, using the default config. In order to specify a config file use 'redis-server /path/to/redis.conf'");
    }
    if (server.daemonize) daemonize();
    initServer();
    if (server.daemonize) createPidFile();
    redisAsciiArt();
    redisLog(REDIS_WARNING,"Server started, Redis version " REDIS_VERSION);
#ifdef __linux__
    linuxOvercommitMemoryWarning();
#endif
    start = ustime();
    if (server.appendonly) {
        if (loadAppendOnlyFile(server.appendfilename) == REDIS_OK)
            redisLog(REDIS_NOTICE,"DB loaded from append only file: %.3f seconds",(float)(ustime()-start)/1000000);
    } else {
        if (rdbLoad(server.dbfilename) == REDIS_OK) {
            redisLog(REDIS_NOTICE,"DB loaded from disk: %.3f seconds",
                (float)(ustime()-start)/1000000);
        } else if (errno != ENOENT) {
            redisLog(REDIS_WARNING,"Fatal error loading the DB. Exiting.");
            exit(1);
        }
    }
    if (server.ipfd > 0)
        redisLog(REDIS_NOTICE,"The server is now ready to accept connections on port %d", server.port);
    if (server.sofd > 0)
        redisLog(REDIS_NOTICE,"The server is now ready to accept connections at %s", server.unixsocket);
    aeSetBeforeSleepProc(server.el,beforeSleep);
    aeMain(server.el);
    aeDeleteEventLoop(server.el);
    return 0;
}

/* The End */
