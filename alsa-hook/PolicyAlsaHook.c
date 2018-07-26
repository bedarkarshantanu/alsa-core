/*
 * Copyright (C) 2016 "IoT.bzh"
 * Author Fulup Ar Foll <fulup@iot.bzh>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * AfbCallBack (snd_ctl_hal_t *handle, int numid, void **response);
 *  AlsaHookInit is mandatory and called with numid=0
 *
 * Syntax in .asoundrc file
 *   CrlLabel    { cb MyFunctionName name "My_Second_Control" }
 *
 * Testing:
 *   aplay -DAlsaHook /usr/share/sounds/alsa/test.wav
 *
 * References:
 *  https://www.spinics.net/lists/alsa-devel/msg54235.html
 *  https://github.com/shivdasgujare/utilities/blob/master/nexuss/alsa-scenario-hook/src/alsa-wrapper.c
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <alloca.h>

#include <alsa/asoundlib.h>
#include <alsa/conf.h>
#include <alsa/pcm.h>

#include <systemd/sd-event.h>
#include <json-c/json.h>

#include "afb/afb-wsj1.h"
#include "afb/afb-ws-client.h"
#include "afb/afb-proto-ws.h"

#include <pthread.h>
#include <semaphore.h>

#define PLUGIN_ENTRY_POINT AlsaInstallHook
    // Fulup Note: What ever you may find on Internet you should use
    // SND_CONFIG_DLSYM_VERSION_HOOK and not SND_CONFIG_DLSYM_VERSION_HOOK
    SND_DLSYM_BUILD_VERSION(PLUGIN_ENTRY_POINT, SND_PCM_DLSYM_VERSION)

// this should be more than enough
#define MAX_API_CALL 10
#define MAX_EVT_CALL 10

// timeout in ms
#define REQUEST_DEFAULT_TIMEOUT 500
#ifndef MAINLOOP_WATCHDOG
#define MAINLOOP_WATCHDOG 100000
#endif

// closing message is added to query when PCM is closed
#define CLOSING_MSG ",\"source\":-1}"

// Currently not implemented
#define UNUSED_ARG(x) UNUSED_ ## x __attribute__((__unused__))
#define  MAGIC_HOOK 22562935

typedef struct {
    char *apiverb;
    json_object *queryJ;
    sd_event_source *evtSource;
    char *callIdTag;
    void *afbClient;
} afbRequestT;

typedef struct {
    char *search;
    char *value;
    long ivalue;
    int signal;
} afbEventT;

typedef struct {
    long magic;
    char *name;
    char *uid;
    snd_pcm_t *pcm;
    char *uri;
    struct afb_proto_ws *pws;
    sd_event *sdLoop;
    int verbose;
    int synchronous;
    sem_t semaphore;
    long timeout;
    int count;
    int errcount;
    afbRequestT **request;
    afbRequestT **release;
    afbEventT **event;
    pthread_t tid;
    json_object * streamIdJ;
} afbClientT;


static void *LoopInThread(void *handle) {
    afbClientT *afbClient = (afbClientT*) handle;
    int count=0;
    int watchdog= MAINLOOP_WATCHDOG *1000;

    /* loop until end */
    for (;;) {

        if (afbClient->verbose) printf("ON-MAINLOOP Session=%s ping=%d\n", afbClient->uid, count++);
        int  res = sd_event_run(afbClient->sdLoop, watchdog);

        if ( res < 0 )
        {
            printf("ERROR in LoopInThread \"%i\" Break ON-MAINLOOP errno=%s.\n", res, strerror(errno));
            break;
        }
    }
    pthread_exit(0);
}

// lost connect with the AudioDaemon
static void OnHangupCB(void *handle) {

    afbClientT *afbClient = (afbClientT*) handle;
    SNDERR("(Hoops) Lost Connection to %s", afbClient->uri);

    // try to close PCM when impossible terminate client
    int err = snd_pcm_close (afbClient->pcm);
    if (err) exit(1);
}

typedef enum {
    HOOK_INSTALL,
    HOOK_CLOSE,
} hookActionT;

void OnEventCB(void *handle, const char *event, int evtid, struct json_object *dataJ) {
    afbClientT *afbClient = (afbClientT*) handle;
    afbEventT **afbEvent = afbClient->event;
    json_object *tmpJ;
    int signal=0, index;

    // if no event handler just ignore events
    if (!afbEvent) goto OnErrorExit;

    if (afbClient->verbose) printf("ON-EVENT processing signal event=%s search=%s session=%s\n", event, json_object_get_string(dataJ), afbClient->uid);

    // loop on event/signal mapping table
    for (index=0; afbEvent[index]!= NULL; index++) {
        int done;

        done = json_object_object_get_ex(dataJ,afbEvent[index]->search, &tmpJ);
        if (done) {

            if (afbEvent[index]->value) {
                // search value is a string
                if (json_object_get_type(tmpJ) != json_type_string) goto OnErrorExit;
                const char *value=json_object_get_string(tmpJ);
                if (!strcmp(afbEvent[index]->value, value)) signal=afbEvent[index]->signal;

            } else {

                // search value is an integer
                if (json_object_get_type(tmpJ) != json_type_int) goto OnErrorExit;
                int ivalue=json_object_get_int(tmpJ);
                if (ivalue == afbEvent[index]->ivalue) signal=afbEvent[index]->signal;
            }
            break;
       }
    }
    if (signal) {
        if (afbClient->verbose) printf("ON-EVENT self-signal search=%s signal=%d\n", afbEvent[index]->search, afbEvent[index]->signal);
        if (afbEvent[index]->signal) kill (getpid(), afbEvent[index]->signal);
    }

    return;

OnErrorExit:
    if (afbClient->verbose) SNDERR("ON-EVENT Fail/Ignored %s(%s)\n", event, json_object_get_string(dataJ));
    return;
}

static void OnSuccessCB(void* UNUSED_ARG(ctx) , void* handle, json_object* responseJ, const char* info) {

    afbRequestT *afbRequest= (afbRequestT*)handle;
    afbClientT *afbClient=(afbClientT*)afbRequest->afbClient;

    if (afbClient->verbose) printf("OnSuccessCB callid='%s' response='%s' info='%s'\n", afbRequest->callIdTag, json_object_get_string(responseJ), info);

    // as pulse does not close PCM even timeout=0 session is not enough and we have to handle stream_id manually
    if (json_object_object_get_ex(responseJ, "stream_id", &afbClient->streamIdJ) && afbClient->verbose) {
        json_object_get(afbClient->streamIdJ);
        printf("OnSuccessCB session store stream_id='%s'\n", json_object_get_string(afbClient->streamIdJ));
    }

    // Cancel timeout for this request
    sd_event_source_unref(afbRequest->evtSource);
    free(afbRequest->callIdTag);

    // When not more waiting call release semaphore
    afbClient->count--;
    if (afbClient->synchronous) {
        if (afbClient->verbose) printf("OnSuccessCB One Request Done\n");
        sem_post (&afbClient->semaphore);
    } else if (afbClient->count == 0) {
        if (afbClient->verbose) printf("OnSuccessCB No More Waiting Request\n");
        sem_post (&afbClient->semaphore);
    }
}

static void OnFailureCB(void* UNUSED_ARG(ctx), void* handle, const char *status, const char *info) {

    afbRequestT *afbRequest= (afbRequestT*)handle;
    afbClientT *afbClient=(afbClientT*)afbRequest->afbClient;

    if (afbClient->verbose) printf("OnFailureCB callid='%s' status='%s' info='%s'\n", afbRequest->callIdTag, status, info);

    // Cancel timeout for this request
    sd_event_source_unref(afbRequest->evtSource);
    free(afbRequest->callIdTag);

    afbClient->errcount++;
    sem_post (&afbClient->semaphore);
}

#if defined(AFB_PROTO_WS_VERSION) && (AFB_PROTO_WS_VERSION >= 3)
static void OnReplyCB(void* ctx, void* handle, json_object* responseJ, const char *error, const char *info) {
    if (error)
        OnFailureCB(ctx, handle, error, info);
    else
        OnSuccessCB(ctx , handle, responseJ, info);
}
#endif

/* the callback interface for pws */
static struct afb_proto_ws_client_itf itf = {
#if defined(AFB_PROTO_WS_VERSION) && (AFB_PROTO_WS_VERSION >= 3)
    .on_reply = OnReplyCB,
#else
    .on_reply_success = OnSuccessCB,
    .on_reply_fail = OnFailureCB,
    .on_subcall = NULL,
#endif
    .on_event_create = NULL,
    .on_event_remove = NULL,
    .on_event_subscribe = NULL,
    .on_event_unsubscribe = NULL,
    .on_event_push = OnEventCB,
    .on_event_broadcast = NULL,
};

int OnTimeoutCB (sd_event_source* source, uint64_t timer, void* handle) {
    afbClientT *afbClient= (afbClientT*)handle;

    SNDERR("\nON-TIMEOUT Call Request Fail session=%s\n", afbClient->uid );

    // Close PCM and release waiting client
    afbClient->errcount=1;
    sem_post (&afbClient->semaphore);

    return 0;
}

// Call AGL binder asynchronously by with a timeout
static int CallWithTimeout(afbClientT *afbClient, afbRequestT *afbRequest, int count) {
    uint64_t usec;
    json_object *queryJ;
    int err;

    // create a unique tag for request
    if(asprintf(&afbRequest->callIdTag, "%d:%s", count, afbRequest->apiverb) < 0) {
        printf("Couldn't allocate request call unique id tag string\n");
        goto OnErrorExit;
    }

    // create a timer with ~250us accuracy
    sd_event_now(afbClient->sdLoop, CLOCK_MONOTONIC, &usec);
    sd_event_add_time(afbClient->sdLoop, &afbRequest->evtSource, CLOCK_MONOTONIC, usec+afbClient->timeout*1000, 250, OnTimeoutCB, afbClient);

    // if steamId is set then add it to api query
    if (!afbClient->streamIdJ) {
       queryJ=afbRequest->queryJ;
    } else {
       queryJ=json_object_new_object();
       json_object_object_foreach(afbRequest->queryJ, key, obj) {
         json_object_object_add(queryJ, key, obj);
       }
        json_object_object_add(queryJ,"stream_id",afbClient->streamIdJ);
    }

    // release action is optional
    if (afbRequest->apiverb) {
        if (afbClient->verbose)  printf("CALL-REQUEST verb=%s query=%s tag=%s session=%s\n", afbRequest->apiverb, json_object_get_string(queryJ), afbRequest->callIdTag, afbClient->uid);
        err = afb_proto_ws_client_call(afbClient->pws, afbRequest->apiverb, queryJ, afbClient->uid, afbRequest
#if defined(AFB_PROTO_WS_VERSION) && (AFB_PROTO_WS_VERSION >= 3)
			, NULL
#endif
		);
        if (err < 0 ) goto OnErrorExit;
    }

    // save client handle in request
    afbRequest->afbClient = afbClient;
    afbClient->count ++;

    // in synchronous mode we wait for CB to return
    if (afbClient->synchronous) {
        sem_wait(&afbClient->semaphore);
        if (afbClient->errcount) {
            printf("CALL-REQUEST FAILED afbClient->errcount=%d session=%s \n", afbClient->errcount,afbClient->uid);
            goto OnErrorExit;
        }
    }

    return 0;

OnErrorExit:
    fprintf(stderr, "LaunchCallRequest: Fail ws-client=%s query=%s&%s\n", afbClient->uri, afbRequest->apiverb, json_object_get_string(afbRequest->queryJ));
    return 1;
}

static int LaunchCallRequest(afbClientT *afbClient, hookActionT action) {
    int err, idx;

    // each callback on error add one to error count.

    afbClient->errcount = 0;

    switch (action) {
        case HOOK_INSTALL: {
            if (afbClient->verbose) printf("HOOK_INSTALL Session=%s\n", afbClient->uid);
            // init waiting counting semaphore
            if (sem_init(&afbClient->semaphore, 1, 0) == -1) {
               fprintf(stderr, "LaunchCallRequest: Fail Semaphore Init: %s\n", afbClient->uri);
            }

            // Create a main loop
            err = sd_event_new(&afbClient->sdLoop);
            if (err < 0) {
                fprintf(stderr, "LaunchCallRequest: Connection to default event loop failed: %s\n", strerror(-err));
                goto OnErrorExit;
            }

            // start a thread with a mainloop to monitor Audio-Agent
            err = pthread_create(&afbClient->tid, NULL, &LoopInThread, afbClient);
            if (err) goto OnErrorExit;

            afbClient->pws = afb_ws_client_connect_api(afbClient->sdLoop, afbClient->uri, &itf, afbClient);
            if (afbClient->pws == NULL) {
                fprintf(stderr, "LaunchCallRequest: Connection to %s failed\n", afbClient->uri);
                goto OnErrorExit;
            }
            if (afbClient->verbose) printf ("LaunchCallRequest:optional HOOK_OPEN uri=%s\n", afbClient->uri);

            // register hanghup callback
            afb_proto_ws_on_hangup(afbClient->pws, OnHangupCB);


            // If no request exit now
            if (!afbClient->release) {
                fprintf(stderr, "LaunchCallRequest: HOOK_INSTALL:fatal mandatory request call missing in asoundrc\n");
                goto OnErrorExit;;
            }

            // send call request to audio-agent asynchronously (respond with thread mainloop context)
            for (idx = 0; afbClient->request[idx] != NULL; idx++) {
                err = CallWithTimeout(afbClient, afbClient->request[idx], idx);
                if (err) goto OnErrorExit;
            }
            // launch counter to keep track of waiting request call
            afbClient->count=idx;
            break;
        }

        case HOOK_CLOSE: {
            if (afbClient->verbose) printf("HOOK_CLOSE Session=%s\n", afbClient->uid);
            // If no request exit now
            if (!afbClient->release) {
                if (afbClient->verbose) printf("LaunchCallRequest:optional HOOK_CLOSE no release control call\n");
                break;
            }

            // send call request to audio-agent asynchronously (respond with thread mainloop context)
            for (idx = 0; afbClient->release[idx] != NULL; idx++) {
                err = CallWithTimeout(afbClient, afbClient->release[idx], idx);
                if (err) goto OnErrorExit;
            }
            // launch counter to keep track of waiting request call
            afbClient->count=idx;
            break;
        }

    default:
       fprintf(stderr, "LaunchCallRequest (hoops): Unknown action");
       goto OnErrorExit;
    }

    return 0;

OnErrorExit:
    return 1;
}

static void AlsaHookClean (afbClientT *afbClient)
{
    free(afbClient->uid);
    free(afbClient->name);
    free(afbClient->uri);
    if (afbClient->event) {
        afbEventT **afbEvent=afbClient->event;
        for (int index=0; afbEvent[index]!= NULL; index++) {
            free(afbEvent[index]->search);
            free(afbEvent[index]->value);
        }
        free(afbEvent);
    }
    if (afbClient->request) {
        afbRequestT **afbRequest=afbClient->request;
        for (int index=0; afbRequest[index]!= NULL; index++) {
            free(afbRequest[index]->apiverb);
            json_object_put(afbRequest[index]->queryJ);
        }
        free(afbRequest);
    }
    if (afbClient->release) {
        afbRequestT **afbRelease=afbClient->release;
        for (int index=0; afbRelease[index]!= NULL; index++) {
            free(afbRelease[index]->apiverb);
            json_object_put(afbRelease[index]->queryJ);
        }
        free(afbRelease);
    }
    afbClient->magic=0;

    // free systemd loop
    if (afbClient->sdLoop) sd_event_unref(afbClient->sdLoop);
    if (afbClient->streamIdJ) json_object_put(afbClient->streamIdJ);
    // close binder websocket
    //ISSUE for Jose
    //if (afbClient->pws) afb_proto_ws_unref(afbClient->pws);
    free(afbClient);
}

static int AlsaCloseHook(snd_pcm_hook_t *hook) {

    afbClientT *afbClient = (afbClientT*) snd_pcm_hook_get_private (hook);
    //hook is already close
    if (!afbClient || afbClient->magic != MAGIC_HOOK ) {
        if (afbClient->verbose) printf("AlsaCloseHook Ignored, invalid afbClient\n");
        return 0;
    }
    // launch call request and create a waiting mainloop thread
    int err = LaunchCallRequest(afbClient, HOOK_CLOSE);
    if (err < 0) {
        fprintf (stderr, "Error on PCM Release Call\n");
        goto OnErrorExit;
    }

    // wait for all call request to return
    if (afbClient->errcount) {
        fprintf (stderr, "AlsaCloseHook: Notice exit before audio-4a response\n");
        goto OnErrorExit;
    }

    // request main loop to terminate without signal
    if (afbClient->verbose) printf("AlsaCloseHook: Start pthread_cancel request\n");
    int s = pthread_cancel(afbClient->tid);
    if (s != 0)
       printf("AlsaCloseHook: ERROR on pthread_cancel err %d\n",s);

    /* Join with thread to see what its exit status was */
    void *res;
    s = pthread_join(afbClient->tid, &res);
    if (s != 0)
        printf("AlsaCloseHook: ERROR on pthread_join err %d\n",s);

    if (res == PTHREAD_CANCELED) {
        if (afbClient->verbose) fprintf(stdout, "\nAlsaHook Close Success PCM=%s URI=%s\n", snd_pcm_name(afbClient->pcm), afbClient->uri);
    } else {
        printf("AlsaCloseHook: EventLoop thread failed to canceled (shouldn't happen!)\n");
    }

    AlsaHookClean(afbClient);
    snd_pcm_hook_set_private(hook,NULL);
    return 0;

OnErrorExit:
    fprintf(stderr, "\nAlsaPcmHook Plugin Close Fail PCM=%s\n", snd_pcm_name(afbClient->pcm));
    return 0;
}

// Get an asoundrc compound and return it as a JSON object
static json_object* AlsaGetJson (snd_config_t *ctlconfig, const char *id, const char*label) {

    json_object *queryJ;
    snd_config_type_t ctype;
    char* query;

    // each control is a sting that contain a json object
    ctype = snd_config_get_type(ctlconfig);
    snd_config_get_string(ctlconfig,  (const char**) &query);
    if (ctype != SND_CONFIG_TYPE_STRING) {
        SNDERR("Invalid signal number for %s value=%s", label, query);
        goto OnErrorExit;
    }

    // cleanup string for json_tokener
    for (int idx = 0; query[idx] != '\0'; idx++) {
        if (query[idx] == '\'') query[idx] = '"';
    }
    queryJ = json_tokener_parse(query);
    if (!queryJ) {
        SNDERR("Not a valid Json object control='%s' query='%s'", id, query);
        goto OnErrorExit;
    }

    return queryJ;

OnErrorExit:
    return NULL;
}

static int AlsaGetActions (snd_config_t *node, afbRequestT **afbRequest, const char*id) {
    const char *callConf, *apiverb;
    snd_config_type_t ctype;
    snd_config_iterator_t currentCall, follow;
    int callCount=0;

    ctype = snd_config_get_type(node);
    if (ctype != SND_CONFIG_TYPE_COMPOUND) {
        snd_config_get_string(node, &callConf);
        SNDERR("Invalid compound type for %s", callConf);
        goto OnErrorExit;
    }


    // loop on each call
    snd_config_for_each(currentCall, follow, node) {
        snd_config_t *ctlconfig = snd_config_iterator_entry(currentCall);

        // ignore empty line
        if (snd_config_get_id(ctlconfig, &apiverb) < 0) continue;

        // allocate an empty call request
        afbRequest[callCount] = calloc(1, sizeof (afbRequestT));
        afbRequest[callCount]->apiverb=strdup(apiverb);
        afbRequest[callCount]->queryJ= AlsaGetJson(ctlconfig, id, apiverb);
        if (!afbRequest[callCount]->queryJ) goto OnErrorExit;

        // move to next call if any
        callCount ++;
        if (callCount == MAX_EVT_CALL) {
            SNDERR("Too Many call MAX_EVT_CALL=%d", MAX_EVT_CALL);
            goto OnErrorExit;
        }
        afbRequest[callCount]=NULL; // afbEvent array is NULL terminated
    }

    return 0;

OnErrorExit:
    return 1;
}

static int AlsaGetEvents (snd_config_t *node, afbEventT **afbEvent, const char*id) {

    const char *confEvents, *evtpattern;
    snd_config_type_t ctype;
    snd_config_iterator_t currentEvt, follow;
    snd_config_t *itemConf;
    int callCount=0;
    int err;

    ctype = snd_config_get_type(node);
    if (ctype != SND_CONFIG_TYPE_COMPOUND) {
        snd_config_get_string(node, &confEvents);
        SNDERR("Invalid compound type for %s", confEvents);
        goto OnErrorExit;
    }

    // loop on each call
    snd_config_for_each(currentEvt, follow, node) {
        snd_config_t *ctlconfig = snd_config_iterator_entry(currentEvt);

        // ignore empty line
        if (snd_config_get_id(ctlconfig, &evtpattern) < 0) continue;

        // allocate an empty call request
        afbEvent[callCount] = calloc(1, sizeof (afbEventT));

        // extract signal num from config label
        for (int idx=0; evtpattern[idx] != '\0'; idx++) {
            if (evtpattern[idx]=='-' && evtpattern[idx+1] != '\0') {
                int done= sscanf (&evtpattern[idx+1], "%d",&afbEvent[callCount]->signal);
                if (done != 1) {
                    SNDERR("Invalid Signal '%s' definition should be something like Sig-xx", evtpattern);
                    goto OnErrorExit;
                }
                break;
            }
        }

        // extract signal key value search pattern
        ctype = snd_config_get_type(ctlconfig);
        if (ctype != SND_CONFIG_TYPE_COMPOUND) {
            snd_config_get_string(ctlconfig, &confEvents);
            SNDERR("Invalid event search pattern for %s value=%s", evtpattern, confEvents);
            goto OnErrorExit;
        }

        // pattern should have a search
        err = snd_config_search(ctlconfig, "search", &itemConf);
        if (!err) {
            const char *search;
            if (snd_config_get_string(itemConf, &search) < 0) {
                SNDERR("Invalid event/signal 'search' should be a string %s", confEvents);
                goto OnErrorExit;
            }
            afbEvent[callCount]->search=strdup(search);
        } else {
            SNDERR("Missing 'search' from event/signal 'search' from signal definition %s", confEvents);
            goto OnErrorExit;
        }

        // pattern should have a value
        err = snd_config_search(ctlconfig, "value", &itemConf);
        if (!err) {
            const char *value;
            switch (snd_config_get_type(itemConf)) {
                case SND_CONFIG_TYPE_INTEGER:
                    snd_config_get_integer(itemConf, &afbEvent[callCount]->ivalue);
                    break;

                case  SND_CONFIG_TYPE_STRING:
                    snd_config_get_string(itemConf, &value);
                    afbEvent[callCount]->value=strdup(value);
                    break;
                default:
                    SNDERR("Invalid event/signal 'value' should be a string %s", confEvents);
                    goto OnErrorExit;
            }
        } else {
            SNDERR("Missing 'value' from event/signal 'value' from signal definition %s", confEvents);
            goto OnErrorExit;
        }

        // move to next call if any
        callCount ++;
        if (callCount == MAX_EVT_CALL) {
            SNDERR("Too Many call MAX_EVT_CALL=%d", MAX_EVT_CALL);
            goto OnErrorExit;
        }
        afbEvent[callCount]=NULL; // afbEvent array is NULL terminated
    }
    return 0;

OnErrorExit:
    return 1;
}

// Function call when Plugin PCM is OPEN
int PLUGIN_ENTRY_POINT (snd_pcm_t *pcm, snd_config_t *conf) {
    snd_pcm_hook_t *h_close = NULL;
    snd_config_iterator_t it, next;
    afbClientT *afbClient = calloc(1,sizeof (afbClientT));
    int err;

    // start populating client handle
    afbClient->pcm = pcm;
    afbClient->name= strdup(snd_pcm_name(pcm));
    afbClient->verbose = 0;
    if(asprintf(&afbClient->uid, "hook:%s:%d", afbClient->name, getpid()) < 0) {
        SNDERR("Couldn't allocate client uid string");
        goto OnErrorExit;
    }
    // Get PCM arguments from asoundrc

    printf("HookEntry handle=0x%p pcm=%s\n", afbClient, afbClient->name);
    snd_config_for_each(it, next, conf) {
        snd_config_t *node = snd_config_iterator_entry(it);
        const char *id;

        // ignore comment en empty lines
        if (snd_config_get_id(node, &id) < 0) continue;
        if (strcmp(id, "comment") == 0 || strcmp(id, "hint") == 0) continue;

        if (strcmp(id, "uri") == 0) {
            const char *uri;
            if (snd_config_get_string(node, &uri) < 0) {
                SNDERR("Invalid String for %s", id);
                goto OnErrorExit;
            }
            afbClient->uri=strdup(uri);
            continue;
        }

        if (strcmp(id, "verbose") == 0) {
            afbClient->verbose= snd_config_get_bool(node);
            if (afbClient->verbose < 0) {
                SNDERR("Invalid Boolean for %s", id);
                goto OnErrorExit;
            }
            continue;
        }

        if (strcmp(id, "synchronous") == 0) {
            afbClient->synchronous= snd_config_get_bool(node);
            if (afbClient->synchronous < 0) {
                SNDERR("Invalid Boolean for %s", id);
                goto OnErrorExit;
            }
            continue;
        }

        if (strcmp(id, "timeout") == 0) {
            if (snd_config_get_integer(node, &afbClient->timeout) < 0) {
                SNDERR("Invalid timeout Integer %s", id);
                goto OnErrorExit;
            }
            continue;
        }

        if (strcmp(id, "request") == 0) {
            afbClient->request =  malloc(MAX_API_CALL * sizeof(afbRequestT*));;
            int err= AlsaGetActions(node, afbClient->request , id);
            if (err) goto OnErrorExit;
            continue;
        }

        if (strcmp(id, "release") == 0) {
            afbClient->release =  malloc(MAX_API_CALL * sizeof(afbRequestT*));;
            int err= AlsaGetActions(node, afbClient->release, id);
            if (err) goto OnErrorExit;
            continue;
        }

        if (strcmp(id, "events") == 0) {
            afbClient->event=  malloc(MAX_API_CALL * sizeof(afbRequestT*));;
            int err= AlsaGetEvents(node, afbClient->event, id);
            if (err) goto OnErrorExit;
            continue;
        }
    }

    if (afbClient->verbose) fprintf(stdout, "\nAlsaHook Install Start PCM=%s URI=%s\n", snd_pcm_name(afbClient->pcm), afbClient->uri);

    err = snd_pcm_hook_add(&h_close, afbClient->pcm, SND_PCM_HOOK_TYPE_CLOSE, AlsaCloseHook, afbClient);
    if (err < 0) goto OnErrorExit;
    afbClient->magic=MAGIC_HOOK;
    // launch call request and create a waiting mainloop thread
    err = LaunchCallRequest(afbClient, HOOK_INSTALL);
    if (err < 0) {
        fprintf (stderr, "PCM Fail to Get Authorisation\n");
        goto OnErrorExit;
    }

    // wait for all call request to return
    if (!afbClient->synchronous) sem_wait(&afbClient->semaphore);
    if (afbClient->errcount) {
        fprintf (stderr, "PCM Authorisation Deny from AAAA Controller (AGL Advanced Audio Agent)\n");
        goto OnErrorExit;
    }

    if (afbClient->verbose) fprintf(stdout, "\nAlsaHook Install Success PCM=%s URI=%s\n", afbClient->name, afbClient->uri);
    return 0;

OnErrorExit:
    fprintf(stderr, "\nAlsaPcmHook Plugin Policy Control Fail PCM=%s\n", afbClient->name);
    if (h_close)
        snd_pcm_hook_remove(h_close);

    return 0;
}

