/*
 * Copyright (c) 2003, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "jvmti.h"
#include "agent_common.h"
#include "JVMTITools.h"
#include "jvmti_tools.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef JNI_ENV_ARG

#ifdef __cplusplus
#define JNI_ENV_ARG(x, y) y
#define JNI_ENV_PTR(x) x
#else
#define JNI_ENV_ARG(x,y) x, y
#define JNI_ENV_PTR(x) (*x)
#endif

#endif

#define PASSED 0
#define STATUS_FAILED 2
#define MAX_THREADS 32

typedef struct item *item_t;
struct item {
    item_t next;
    jmethodID method;
    int depth;
} item;

typedef struct thr {
    jthread thread;
    item_t tos;
} thr;

static jvmtiEnv *jvmti = NULL;
static jvmtiCapabilities caps;
static jvmtiEventCallbacks callbacks;
static jrawMonitorID event_lock;
static jint result = PASSED;
static jboolean printdump = JNI_FALSE;
static jboolean watch_events = JNI_FALSE;

static int pop_count = 0;
static int push_count = 0;
static int thr_count = 0;
static int max_depth = 0;
static thr threads[MAX_THREADS];

static
int isTestThread(jvmtiEnv *jvmti_env, jthread thr) {
    jvmtiError err;
    jvmtiThreadInfo inf;
    const char* TEST_THREAD_NAME_BASE = "Test Thread";

    err = (*jvmti_env)->GetThreadInfo(jvmti_env, thr, &inf);
    if (err != JVMTI_ERROR_NONE) {
         printf("(GetThreadInfo) unexpected error: %s (%d)\n", TranslateError(err), err);
         result = STATUS_FAILED;
         return 0;
    }
    return strncmp(inf.name, TEST_THREAD_NAME_BASE, strlen(TEST_THREAD_NAME_BASE)) == 0;
}

static
void printInfo(jvmtiEnv *jvmti_env, jthread thr, jmethodID method, int depth) {
    jvmtiError err;
    jvmtiThreadInfo inf;
    char *clsig, *name, *sig, *generic;
    jclass cls;

    err = (*jvmti_env)->GetThreadInfo(jvmti_env, thr, &inf);
    if (err != JVMTI_ERROR_NONE) {
        printf("(GetThreadInfo) unexpected error: %s (%d)\n",
               TranslateError(err), err);
        result = STATUS_FAILED;
    }

    err = (*jvmti_env)->GetMethodDeclaringClass(jvmti_env, method, &cls);
    if (err != JVMTI_ERROR_NONE) {
        printf("(GetMethodDeclaringClass) unexpected error: %s (%d)\n",
               TranslateError(err), err);
        result = STATUS_FAILED;
    }

    err = (*jvmti_env)->GetClassSignature(jvmti_env, cls, &clsig, &generic);
    if (err != JVMTI_ERROR_NONE) {
        printf("(GetClassSignature) unexpected error: %s (%d)\n",
               TranslateError(err), err);
        result = STATUS_FAILED;
    }

    err = (*jvmti_env)->GetMethodName(jvmti_env, method, &name, &sig, &generic);
    if (err != JVMTI_ERROR_NONE) {
        printf("(GetMethodName) unexpected error: %s (%d)\n",
               TranslateError(err), err);
        result = STATUS_FAILED;
    }

    printf("  %s: %s.%s%s, depth = %d\n", inf.name, clsig, name, sig, depth);

    (*jvmti_env)->Deallocate(jvmti_env, (unsigned char *)sig);
    (*jvmti_env)->Deallocate(jvmti_env, (unsigned char *)name);
    (*jvmti_env)->Deallocate(jvmti_env, (unsigned char *)clsig);
    (*jvmti_env)->Deallocate(jvmti_env, (unsigned char *)inf.name);
}

static
void pop(jvmtiEnv *jvmti_env, JNIEnv *env, jthread thr, jmethodID method, int depth) {
    item_t old;
    int i, count = 0;

    for (i = 0; i < thr_count; i++) {
        if (JNI_ENV_PTR(env)->IsSameObject(JNI_ENV_ARG(env, threads[i].thread),
                thr) == JNI_TRUE) {
            break;
        }
    }

    if (i == thr_count) {
        watch_events = JNI_FALSE;
        printf("Unknown thread:\n");
        printInfo(jvmti_env, thr, method, depth);
        result = STATUS_FAILED;
        return;
    }

    if (threads[i].tos == NULL) {
        watch_events = JNI_FALSE;
        printf("Stack underflow:\n");
        printInfo(jvmti_env, thr, method, depth);
        result = STATUS_FAILED;
        return;
    }

    do {
        pop_count++;
        old = threads[i].tos;
        threads[i].tos = threads[i].tos->next;
        if (old->method == method && old->depth == depth) {
            free(old);
            return;
        }
        free(old);
    } while (threads[i].tos != NULL);

    watch_events = JNI_FALSE;
    printf("Frame pop does not match any entry:\n");
    printInfo(jvmti_env, thr, method, depth);
    result = STATUS_FAILED;
}

static
void push(JNIEnv *env, jthread thr, jmethodID method, int depth) {
    item_t new;
    int i;

    for (i = 0; i < thr_count; i++) {
        if (JNI_ENV_PTR(env)->IsSameObject(JNI_ENV_ARG(env, threads[i].thread),
                thr) == JNI_TRUE) {
            break;
        }
    }

    if (i == thr_count) {
        thr_count++;
        if (thr_count == MAX_THREADS) {
            watch_events = JNI_FALSE;
            printf("Out of threads\n");
            result = STATUS_FAILED;
            return;
        }
        threads[i].thread = (*env)->NewGlobalRef(env, thr);
        threads[i].tos = NULL;
    }

    new = (item_t)malloc(sizeof(item));
    if (new == NULL) {
        watch_events = JNI_FALSE;
        printf("Out of memory\n");
        result = STATUS_FAILED;
        return;
    }

    new->next = threads[i].tos;
    new->method = method;
    new->depth = depth;
    threads[i].tos = new;
    push_count++;
    max_depth = (max_depth < depth) ? depth: max_depth;
}

void JNICALL MethodEntry(jvmtiEnv *jvmti_env, JNIEnv *env,
        jthread thr, jmethodID method) {
    jvmtiError err;
    jboolean isNative;
    jint frameCount;

    if (watch_events == JNI_FALSE) return;

    err = (*jvmti_env)->GetFrameCount(jvmti_env, thr, &frameCount);
    if (err != JVMTI_ERROR_NONE) {
        printf("(GetFrameCount#entry) unexpected error: %s (%d)\n",
               TranslateError(err), err);
        printInfo(jvmti_env, thr, method, frameCount);
        result = STATUS_FAILED;
        return;
    }

    err = (*jvmti_env)->IsMethodNative(jvmti_env, method, &isNative);
    if (err != JVMTI_ERROR_NONE) {
        printf("(IsMethodNative) unexpected error: %s (%d)\n",
               TranslateError(err), err);
        printInfo(jvmti_env, thr, method, frameCount);
        result = STATUS_FAILED;
    }

    if (isTestThread(jvmti_env, thr)) {
        if (printdump == JNI_TRUE) {
            printf(">>> %sMethod entry\n>>>",
                   (isNative == JNI_TRUE) ? "Native " : "");
            printInfo(jvmti_env, thr, method, frameCount);
        }
        if (isNative == JNI_FALSE) {
            err = (*jvmti_env)->RawMonitorEnter(jvmti_env, event_lock);
            if (err != JVMTI_ERROR_NONE) {
                printf("(RawMonitorEnter) unexpected error: %s (%d)\n",
                       TranslateError(err), err);
                printInfo(jvmti_env, thr, method, frameCount);
                result = STATUS_FAILED;
            }
            push((JNIEnv *)env, thr, method, frameCount);
            err = (*jvmti_env)->RawMonitorExit(jvmti_env, event_lock);
            if (err != JVMTI_ERROR_NONE) {
                printf("(RawMonitorExit) unexpected error: %s (%d)\n",
                       TranslateError(err), err);
                printInfo(jvmti_env, thr, method, frameCount);
                result = STATUS_FAILED;
            }
            err = (*jvmti_env)->NotifyFramePop(jvmti_env, thr, 0);
            if (err != JVMTI_ERROR_NONE) {
                printf("(NotifyFramePop) unexpected error: %s (%d)\n",
                       TranslateError(err), err);
                printInfo(jvmti_env, thr, method, frameCount);
                result = STATUS_FAILED;
            }
        }
    }
}

void JNICALL FramePop(jvmtiEnv *jvmti_env, JNIEnv *env,
        jthread thr, jmethodID method, jboolean wasPopedByException) {
    jvmtiError err;
    int frameCount;

    err = (*jvmti_env)->GetFrameCount(jvmti_env, thr, &frameCount);
    if (err != JVMTI_ERROR_NONE) {
        printf("(GetFrameCount#entry) unexpected error: %s (%d)\n",
               TranslateError(err), err);
        printInfo(jvmti_env, thr, method, frameCount);
        result = STATUS_FAILED;
        return;
    }

    if (isTestThread(jvmti_env, thr)) {
        if (printdump == JNI_TRUE) {
            printf(">>> Frame Pop\n>>>");
            printInfo(jvmti_env, thr, method, frameCount);
        }
        err = (*jvmti_env)->RawMonitorEnter(jvmti_env, event_lock);
        if (err != JVMTI_ERROR_NONE) {
            printf("(RawMonitorEnter) unexpected error: %s (%d)\n",
                   TranslateError(err), err);
            printInfo(jvmti_env, thr, method, frameCount);
            result = STATUS_FAILED;
        }
        pop(jvmti_env, (JNIEnv *)env, thr, method, frameCount);
        err = (*jvmti_env)->RawMonitorExit(jvmti_env, event_lock);
        if (err != JVMTI_ERROR_NONE) {
            printf("(RawMonitorExit) unexpected error: %s (%d)\n",
                   TranslateError(err), err);
            printInfo(jvmti_env, thr, method, frameCount);
            result = STATUS_FAILED;
        }
    }
}

#ifdef STATIC_BUILD
JNIEXPORT jint JNICALL Agent_OnLoad_framepop002(JavaVM *jvm, char *options, void *reserved) {
    return Agent_Initialize(jvm, options, reserved);
}
JNIEXPORT jint JNICALL Agent_OnAttach_framepop002(JavaVM *jvm, char *options, void *reserved) {
    return Agent_Initialize(jvm, options, reserved);
}
JNIEXPORT jint JNI_OnLoad_framepop002(JavaVM *jvm, char *options, void *reserved) {
    return JNI_VERSION_1_8;
}
#endif
jint Agent_Initialize(JavaVM *jvm, char *options, void *reserved) {
    jvmtiError err;
    jint res;

    if (options != NULL && strcmp(options, "printdump") == 0) {
        printdump = JNI_TRUE;
    }

    res = JNI_ENV_PTR(jvm)->GetEnv(JNI_ENV_ARG(jvm, (void **) &jvmti),
        JVMTI_VERSION_1_1);
    if (res != JNI_OK || jvmti == NULL) {
        printf("Wrong result of a valid call to GetEnv!\n");
        return JNI_ERR;
    }

    err = (*jvmti)->CreateRawMonitor(jvmti, "_event_lock", &event_lock);
    if (err != JVMTI_ERROR_NONE) {
        printf("(CreateRawMonitor) unexpected error: %s (%d)\n",
               TranslateError(err), err);
        return JNI_ERR;
    }

    err = (*jvmti)->GetPotentialCapabilities(jvmti, &caps);
    if (err != JVMTI_ERROR_NONE) {
        printf("(GetPotentialCapabilities) unexpected error: %s (%d)\n",
               TranslateError(err), err);
        return JNI_ERR;
    }

    err = (*jvmti)->AddCapabilities(jvmti, &caps);
    if (err != JVMTI_ERROR_NONE) {
        printf("(AddCapabilities) unexpected error: %s (%d)\n",
               TranslateError(err), err);
        return JNI_ERR;
    }

    err = (*jvmti)->GetCapabilities(jvmti, &caps);
    if (err != JVMTI_ERROR_NONE) {
        printf("(GetCapabilities) unexpected error: %s (%d)\n",
               TranslateError(err), err);
        return JNI_ERR;
    }

    if (caps.can_generate_frame_pop_events &&
            caps.can_generate_method_entry_events) {
        callbacks.MethodEntry = &MethodEntry;
        callbacks.FramePop = &FramePop;
        err = (*jvmti)->SetEventCallbacks(jvmti, &callbacks, sizeof(callbacks));
        if (err != JVMTI_ERROR_NONE) {
            printf("(SetEventCallbacks) unexpected error: %s (%d)\n",
                   TranslateError(err), err);
            return JNI_ERR;
        }
    } else {
        printf("Warning: FramePop or MethodEntry event is not implemented\n");
    }

    return JNI_OK;
}

JNIEXPORT void JNICALL Java_nsk_jvmti_FramePop_framepop002_getReady(JNIEnv *env, jclass cls) {
    jvmtiError err;

    if (!caps.can_generate_frame_pop_events ||
            !caps.can_generate_method_entry_events) {
        return ;
    }

    err = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE,
         JVMTI_EVENT_METHOD_ENTRY, NULL);
    if (err != JVMTI_ERROR_NONE) {
        printf("Failed to enable JVMTI_EVENT_METHOD_ENTRY event: %s (%d)\n",
               TranslateError(err), err);
        result = STATUS_FAILED;
    }
    err = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_ENABLE,
         JVMTI_EVENT_FRAME_POP, NULL);
    if (err != JVMTI_ERROR_NONE) {
        printf("Failed to enable JVMTI_EVENT_FRAME_POP event: %s (%d)\n",
               TranslateError(err), err);
        result = STATUS_FAILED;
    }
    watch_events = JNI_TRUE;
}

JNIEXPORT jint JNICALL Java_nsk_jvmti_FramePop_framepop002_check(JNIEnv *env, jclass cls) {
    jvmtiError err;

    watch_events = JNI_FALSE;
    err = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_DISABLE,
         JVMTI_EVENT_FRAME_POP, NULL);
    if (err != JVMTI_ERROR_NONE) {
        printf("Failed to disable JVMTI_EVENT_FRAME_POP event: %s (%d)\n",
               TranslateError(err), err);
        result = STATUS_FAILED;
    }
    err = (*jvmti)->SetEventNotificationMode(jvmti, JVMTI_DISABLE,
         JVMTI_EVENT_METHOD_ENTRY, NULL);
    if (err != JVMTI_ERROR_NONE) {
        printf("Failed to disable JVMTI_EVENT_METHOD_ENTRY event: %s (%d)\n",
               TranslateError(err), err);
        result = STATUS_FAILED;
    }

    if (printdump == JNI_TRUE) {
        printf("%d threads, %d method entrys, %d frame pops, max depth = %d\n",
               thr_count, push_count, pop_count, max_depth);
    }

    return result;
}

#ifdef __cplusplus
}
#endif
