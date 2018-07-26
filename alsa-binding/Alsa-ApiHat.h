/*
 * AlsaLibMapping -- provide low level interface with ALSA lib (extracted from alsa-json-gateway code)
 * Copyright (C) 2015,2016,2017, Fulup Ar Foll fulup@iot.bzh
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
 */


#ifndef ALSALIBMAPPING_H
#define ALSALIBMAPPING_H


#include <alsa/asoundlib.h>
#include <systemd/sd-event.h>

#include <afb/afb-binding.h>
#include <json-c/json.h>

// Soft control have dynamically allocated numid
#define CTL_AUTO -1

#ifndef PUBLIC
  #define PUBLIC
#endif
#define STATIC static

#ifndef CONTROL_MAXPATH_LEN
  #define CONTROL_MAXPATH_LEN 255
#endif

typedef enum {
  QUERY_QUIET   =0,
  QUERY_COMPACT =1,
  QUERY_VERBOSE =2,
  QUERY_FULL    =3,
} queryModeE;

typedef enum {
    INFO_BY_DEVID,
    INFO_BY_PATH
} InfoGetT;

typedef enum {
    ACTION_SET,
    ACTION_GET
} ActionSetGetT;

// generic structure to pass parsed query values
typedef struct {
  const char *devid;
  json_object *numidsJ;
  queryModeE mode;
  int count;
} queryValuesT;

// use to store crl numid user request
typedef struct {
    unsigned int numId;
    const char *tag;
    json_object *jToken;
    json_object *valuesJ;
    int used;
} ctlRequestT;

// import from AlsaAfbBinding
extern const struct afb_binding_interface *afbIface;
PUBLIC json_object *alsaCheckQuery (afb_req_t request, queryValuesT *queryValues);

// AlseCoreSetGet exports
PUBLIC int alsaGetSingleCtl (snd_ctl_t *ctlDev, snd_ctl_elem_id_t *elemId, ctlRequestT *ctlRequest, queryModeE queryMode);
PUBLIC void alsaGetInfo (afb_req_t request);
PUBLIC void alsaGetCtls(afb_req_t request);
PUBLIC void alsaSetCtls(afb_req_t request);


// AlsaUseCase exports
PUBLIC void alsaUseCaseQuery(afb_req_t request);
PUBLIC void alsaUseCaseSet(afb_req_t request);
PUBLIC void alsaUseCaseGet(afb_req_t request);
PUBLIC void alsaUseCaseClose(afb_req_t request);
PUBLIC void alsaUseCaseReset(afb_req_t request);
PUBLIC void alsaAddCustomCtls(afb_req_t request);

// AlsaRegEvt
PUBLIC void alsaEvtSubcribe (afb_req_t request);
PUBLIC void alsaGetCardId (afb_req_t request);
PUBLIC void alsaRegisterHal (afb_req_t request);
PUBLIC void alsaActiveHal (afb_req_t request);
PUBLIC void alsaPcmInfo (afb_req_t request);

#endif /* ALSALIBMAPPING_H */

