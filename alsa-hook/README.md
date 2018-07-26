Alsa-Hook-Plugin

Object: Provide a Hook on Alsa PCM to check permission again AGL Advance Audio Agent
Status: Release Candidate
Author: Fulup Ar Foll fulup@iot.bzh
Date  : August-2017
 References: https://www.spinics.net/lists/alsa-devel/msg54235.html


## Functionalities:
 - Execute a set of unix/ws RPC request again AGL binders to allow/deny access
 - Keep websocket open in an idependant thread in order to monitor event received from AGL audio agent

## Installation
 - Alsaplugins are typically search in /usr/share/alsa-lib. Nevertheless a full path might be given
 - This plugin implement a hook on a slave PCM. Typically this slave PCM is a dedicated virtual channel (eg: navigation, emergency,...)
 - Config should be place in ~/.asoundrc (see config sample in PROJECT_ROOT/conf.d/alsa)

## Test
 - Install a full .asoundrc from conf.d/project/alsa.d
 - Check SoundCard ==> speaker-test -Dhw:v1340   -c2 -twav
 - Check MixerPCM  ==> speaker-test -DSpeakers   -c2 -twav 
 - Check SoftVol   ==> speaker-test -DMusicPCM   -c2 -twav 
 - Check Plugin    ==> speaker-test -DMultimedia -c2 -twav
 - Check MultiMedia aplay -DDMyNavPCM /usr/share/sounds/alsa/test.wav


## Pulse audio integration

By default Pulse only auto detect hardware sound card and virtual channel should be specified

    1) update ~/.asoundrc file and check basic ALSA virtual channel works as expected
    2) kill/restart pulseaudio server to force alsa hookplugin loading
    3) declare alsa virtual channel as pulse sink
       - pacmd load-module module-alsa-sink device=MusicPCM
       - pacmd load-module module-alsa-sink device=NavPCM control=NavAlsaCtl
       - pacmd list-sinks | grep -i music
    4) check your virtual pulse sink work
       - paplay -d alsa_output.MusicPCM /usr/share/sounds/alsa/test.wav
       - paplay -d alsa_output.NavPCM /usr/share/sounds/alsa/Front_Center.wav

optional integration with ALSA legacy apps

    5) start audio-pol4a controller
       - repeat previous step(3) this time with device=Multimedia device=Navigation, ....
    6) check virtual channel with policy controller protection
       - paplay -d alsa_output.Multimedia /usr/share/sounds/alsa/test.wav
       - paplay -d alsa_output.Navigation /usr/share/sounds/alsa/Front_Center.wav


Bug/Feature: 
    1) when softvol control is initialised from plugin and not
       from AGL binding. At 1st run ctl has invalid TLV and cannot be used
       Bypass Solution: 
         * start audio-binder before playing sound (binding create control before softvol plugin)
         * run a dummy aplay -DMyNavPCM "" to get a clean control
    2) When using Audio-Pol4a to protect virtual channel, the audio policy controller should
       run before adding virtual channel into pulse.
    3) In order to leverage SMACK to protect virtual audio role, a Pulse rooting plugin should be used.


## Alsa Config Sample

```
# ------------------------------------------------------
# Mixer PCM allow to play multiple stream simultaneously
# ------------------------------------------------------
pcm.Speakers { 
    type dmix            
    slave {pcm "hw:v1340"}  #Jabra Solmate 1
    ipc_key 1001          # ipc_key should be unique to each dmix
} 

# -----------------------------------------------------
#  Register ControllerHookPlugin (ToiBeFix fullpath)
# -----------------------------------------------------
pcm_hook_type.CtlHookPlugin {
    install "AlsaInstallHook" 
    lib "/home/fulup/Workspace/Audio-4a/alsa-4a/build/alsa-hook/policy_alsa_hook.so"
}


# -------------------------------------------------------
# Define one Audio Virtual Channel per Audio Roles
# -------------------------------------------------------
pcm.MusicPCM {
    type softvol

    # Point Slave on HOOK for policies control
    slave.pcm "Speakers"

    # name should match with HAL definition
    control.name  "Playback Multimedia"
}

pcm.NaviPCM {
    type softvol

    # Point Slave on HOOK for policies control
    slave.pcm "Speakers"

    # name should match with HAL definition
    control.name  "Playback Navigation"
}

pcm.UrgentPCM {
    type softvol

    # Point Slave on HOOK for policies control
    slave.pcm "Speakers"

    # name should match with HAL definition
    control.name  "Playback Emergency"
}

# ----------------------------------------------------
# Define one hooked PCM channel per Audio Roles
# ----------------------------------------------------
pcm.Multimedia {
    type hooks
    slave {pcm "MusicPCM"}
    hooks.0 {
        comment "Defined used hook sharelib and provide arguments/config to install func"
        type "CtlHookPlugin"
        hook_args {

            # print few log messages (default false)
            verbose true 

            # uri to audio-4a policy engine
            uri="unix:/var/tmp/ahl-4a"

            # timeout in ms (default 500)
            timeout 5000

            # force API synchronous mode
            synchronous true

            # api subcall to request a role
            request {
                open_stream "{'role': 'entertainment'}"
                set-stream "{'role': 'entertainment'}"
            } 

            # api subcall to request a role
            release {
                close-stream "{'role': 'entertainment'}"
            } 
   
            # map AGL event on Unix signal. Search in event for json key=value
            events {   
                sig-02 {search state_event, value 1}
                sig-31 {search state_event, value 2}
                sig-32 {search state_event, value 3}
            }
        }
    }
}

pcm.Navigation {
    type hooks
    slave {pcm "NaviPCM"}
    hooks.0 {
        comment "Defined used hook sharelib and provide arguments/config to install func"
        type "CtlHookPlugin"
        hook_args {

            # print few log messages (default false)
            verbose true 

            # uri to audio-4a policy engine
            uri="unix:/var/tmp/pol4a"

            # timeout in ms (default 500)
            timeout 5000

            # force API synchronous mode
            synchronous true

            # api subcall to request a role
            request {
                navigation-role "{'uid':'alsa-hook-client'}"
                signal-timeout  "{'timeout':5, 'navi':'quit'}"
            } 

            # api subcall to request a role
            release {
                release-role "{'uid':'alsa-hook-client'}"
            } 
   
            # map AGL event on Unix signal. Search in event for json key=value
            events {   
                sig-02 {search navi, value quit}
                sig-31 {search event, value start}
                sig-32 {search event, value start}
            }
        }
    }
}


```

NOTE:

* Hook plugin is loaded by Alsa libasound within client context. It inherits client process attributes, as UID/GID and
SMACK label when running on AGL. The smack label is control by AGL security framework.
As a result a control request succeeds only when client application permission match requested audio role inside Cynara security database.

* Hook plugin keep a connection with the Audio-Agent until PCM is closed by the application. This connection allow the
Audio-Agent to send events. eg: pause, quit, mute, ...