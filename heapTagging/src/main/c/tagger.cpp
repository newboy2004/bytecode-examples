#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include "jvmti.h"
#include "jni.h"
#pragma GCC diagnostic ignored "-Wwrite-strings"

typedef struct {
	/* JVMTI Environment */
	jvmtiEnv *jvmti;
	JNIEnv * jni;
	jboolean vm_is_started;
	jboolean vmDead;

	/* Data access Lock */
	jrawMonitorID lock;
	JavaVM* jvm;
} GlobalAgentData;
typedef struct DeleteQueue {
	jobject obj;
	DeleteQueue * next;
} DeleteQueue;

//Queue of global references that need to be cleaned up
static DeleteQueue * deleteQueue = NULL;
static jrawMonitorID deleteQueueLock;

static GlobalAgentData *gdata;

void fatal_error(const char * format, ...) {
	va_list ap;

	va_start(ap, format);
	(void) vfprintf(stderr, format, ap);
	(void) fflush(stderr);
	va_end(ap);
	exit(3);
}

static void check_jvmti_error(jvmtiEnv *jvmti, jvmtiError errnum,
		const char *str) {
	if (errnum != JVMTI_ERROR_NONE) {
		char *errnum_str;

		errnum_str = NULL;
		(void) jvmti->GetErrorName(errnum, &errnum_str);

		printf("ERROR: JVMTI: %d(%s): %s\n", errnum,
				(errnum_str == NULL ? "Unknown" : errnum_str),
				(str == NULL ? "" : str));
	}
}
/* Enter a critical section by doing a JVMTI Raw Monitor Enter */
static void enter_critical_section(jvmtiEnv *jvmti) {
	jvmtiError error;

	error = jvmti->RawMonitorEnter(gdata->lock);
	check_jvmti_error(jvmti, error, "Cannot enter with raw monitor");
}

/* Exit a critical section by doing a JVMTI Raw Monitor Exit */
static void exit_critical_section(jvmtiEnv *jvmti) {
	jvmtiError error;

	error = jvmti->RawMonitorExit(gdata->lock);
	check_jvmti_error(jvmti, error, "Cannot exit with raw monitor");
}

/*
 * Implementation of _setTag JNI function.
 */
JNIEXPORT static void JNICALL setObjExpression(JNIEnv *env, jclass klass,
		jobject o, jobject expr) {
	if (gdata->vmDead) {
		return;
	}
	if(!o)
	{
		return;
	}
	jvmtiError error;
	jlong tag;
	if (expr) {
		//First see if there's already something set here
		error =gdata->jvmti->GetTag(o,&tag);
		if(tag)
		{
			//Delete reference to old thing
			env->DeleteGlobalRef((jobject)(ptrdiff_t) tag);
		}
		//Set the tag, make a new global reference to it
		error = gdata->jvmti->SetTag(o, (jlong) (ptrdiff_t) (void*) env->NewGlobalRef(expr));
	} else {
		error = gdata->jvmti->SetTag(o, 0);
	}
	if(error == JVMTI_ERROR_WRONG_PHASE)
	return;
	check_jvmti_error(gdata->jvmti, error, "Cannot set object tag");
}
/*
 * Implementation of _getTag JNI function
 */
JNIEXPORT static jobject JNICALL getObjExpression(JNIEnv *env, jclass klass,
		jobject o) {
	if (gdata->vmDead) {
		return NULL;
	}
	jvmtiError error;
	jlong tag;
	error = gdata->jvmti->GetTag(o, &tag);
	if(error == JVMTI_ERROR_WRONG_PHASE)
	return NULL;
	check_jvmti_error(gdata->jvmti, error, "Cannot get object tag");
	if(tag)
	{
		return (jobject) (ptrdiff_t) tag;
	}
	return NULL;
}

/*
 * Since we create a global reference to whatever we tag an object with, we need to clean this up
 * when the tagged object is garbage collected - otherwise tags wouldn't ever be garbage collected.
 * When a tagged object is GC'ed, we add its tag to a deletion queue. We will process the queue at the next GC.
 */
static void JNICALL
cbObjectFree(jvmtiEnv *jvmti_env, jlong tag) {
	if (gdata->vmDead) {
		return;
	}
	jvmtiError error;
	if (tag) {
		error = gdata->jvmti->RawMonitorEnter(deleteQueueLock);
		check_jvmti_error(jvmti_env, error, "raw monitor enter");
		DeleteQueue* tmp = deleteQueue;
		deleteQueue = new DeleteQueue();
		deleteQueue->next = tmp;
		deleteQueue->obj = (jobject) (ptrdiff_t) tag;
		error = gdata->jvmti->RawMonitorExit(deleteQueueLock);
		check_jvmti_error(jvmti_env, error, "raw monitor exit");
	}
}
static jrawMonitorID gcLock;
static int gc_count;

/*
 * Garbage collection worker thread that will asynchronously free tags
 */
static void JNICALL
gcWorker(jvmtiEnv* jvmti, JNIEnv* jni, void *p)
{
	jvmtiError err;
	for (;;) {
		err = jvmti->RawMonitorEnter(gcLock);
		check_jvmti_error(jvmti, err, "raw monitor enter");
		while (gc_count == 0) {
			err = jvmti->RawMonitorWait(gcLock, 0);
			if (err != JVMTI_ERROR_NONE) {
				err = jvmti->RawMonitorExit(gcLock);
				check_jvmti_error(jvmti, err, "raw monitor wait");
				return;
			}
		}
		gc_count = 0;

		err = jvmti->RawMonitorExit(gcLock);
		check_jvmti_error(jvmti, err, "raw monitor exit");

		DeleteQueue * tmp;
		while(deleteQueue)
		{
			err = jvmti->RawMonitorEnter(deleteQueueLock);
			check_jvmti_error(jvmti, err, "raw monitor enter");

			tmp = deleteQueue;
			deleteQueue = deleteQueue->next;
			err = jvmti->RawMonitorExit(deleteQueueLock);
			check_jvmti_error(jvmti, err, "raw monitor exit");
			jni->DeleteGlobalRef(tmp->obj);

			free(tmp);
		}
	}
}

/*
 * Callback to notify us when a GC finishes. When a GC finishes,
 * we wake up our GC thread and free all tags that need to be freed.
 */
static void JNICALL
gc_finish(jvmtiEnv* jvmti_env)
{
	jvmtiError err;
	err = gdata->jvmti->RawMonitorEnter(gcLock);
	check_jvmti_error(gdata->jvmti, err, "raw monitor enter");
	gc_count++;
	err = gdata->jvmti->RawMonitorNotify(gcLock);
	check_jvmti_error(gdata->jvmti, err, "raw monitor notify");
	err = gdata->jvmti->RawMonitorExit(gcLock);
	check_jvmti_error(gdata->jvmti, err, "raw monitor exit");
}
/*
 * Create a new java.lang.Thread
 */
static jthread alloc_thread(JNIEnv *env) {
	jclass thrClass;
	jmethodID cid;
	jthread res;

	thrClass = env->FindClass("java/lang/Thread");
	if (thrClass == NULL) {
		fatal_error("Cannot find Thread class\n");
	}
	cid = env->GetMethodID(thrClass, "<init>", "()V");
	if (cid == NULL) {
		fatal_error("Cannot find Thread constructor method\n");
	}
	res = env->NewObject(thrClass, cid);
	if (res == NULL) {
		fatal_error("Cannot create new Thread object\n");
	}
	return res;
}

/*
 * Callback we get when the JVM is initialized. We use this time to setup our GC thread
 */
static void JNICALL callbackVMInit(jvmtiEnv * jvmti, JNIEnv * env, jthread thread)
{
	jvmtiError err;

	err = jvmti->RunAgentThread(alloc_thread(env), &gcWorker, NULL,
			JVMTI_THREAD_MAX_PRIORITY);
	check_jvmti_error(jvmti, err, "Unable to run agent cleanup thread");
}
/*
 * Callback we receive when the JVM terminates - no more functions can be called after this
 */
static void JNICALL callbackVMDeath(jvmtiEnv *jvmti_env, JNIEnv* jni_env) {
	gdata->vmDead = JNI_TRUE;
}

/*
 * Callback we get when the JVM starts up, but before its initialized.
 * Sets up the JNI calls.
 */
static void JNICALL cbVMStart(jvmtiEnv *jvmti, JNIEnv *env) {

	enter_critical_section(jvmti);
	{
		jclass klass;
		jfieldID field;
		jint rc;

		static JNINativeMethod registry[2] = { {"_setTag",
				"(Ljava/lang/Object;Ljava/lang/Object;)V",
				(void*) &setObjExpression}, {"_getTag",
				"(Ljava/lang/Object;)Ljava/lang/Object;",
				(void*) &getObjExpression}};
		/* Register Natives for class whose methods we use */
		klass = env->FindClass("net/jonbell/examples/jvmti/tagging/runtime/Tagger");
		if (klass == NULL) {
			fatal_error(
					"ERROR: JNI: Cannot find Tagger with FindClass\n");
		}
		rc = env->RegisterNatives(klass, registry, 2);
		if (rc != 0) {
			fatal_error(
					"ERROR: JNI: Cannot register natives for Tagger\n");
		}
		/* Engage calls. */
		field = env->GetStaticFieldID(klass, "engaged", "I");
		if (field == NULL) {
			fatal_error("ERROR: JNI: Cannot get field\n"
			);
		}
		env->SetStaticIntField(klass, field, 1);

		/* Indicate VM has started */
		gdata->vm_is_started = JNI_TRUE;

	}
	exit_critical_section(jvmti);
}

/*
 * Callback that is notified when our agent is loaded. Registers for event
 * notifications.
 */
JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM *jvm, char *options,
		void *reserved) {
	static GlobalAgentData data;
	jvmtiError error;
	jint res;
	jvmtiEventCallbacks callbacks;
	jvmtiEnv *jvmti = NULL;
	jvmtiCapabilities capa;

	(void) memset((void*) &data, 0, sizeof(data));
	gdata = &data;
	gdata->jvm = jvm;
	res = jvm->GetEnv((void **) &jvmti, JVMTI_VERSION_1_0);

	if (res != JNI_OK || jvmti == NULL) {
		/* This means that the VM was unable to obtain this version of the
		 *   JVMTI interface, this is a fatal error.
		 */
		printf("ERROR: Unable to access JVMTI Version 1 (0x%x),"
				" is your J2SE a 1.5 or newer version?"
				" JNIEnv's GetEnv() returned %d\n", JVMTI_VERSION_1, res);

	}
	//save jvmti for later
	gdata->jvmti = jvmti;

	//Register our capabilities
	(void) memset(&capa, 0, sizeof(jvmtiCapabilities));
	capa.can_signal_thread = 1;
	capa.can_generate_object_free_events = 1;
	capa.can_tag_objects = 1;
	capa.can_generate_garbage_collection_events = 1;

	error = jvmti->AddCapabilities(&capa);
	check_jvmti_error(jvmti, error,
			"Unable to get necessary JVMTI capabilities.");

	//Register callbacks
	(void) memset(&callbacks, 0, sizeof(callbacks));
	callbacks.VMInit = &callbackVMInit;
	callbacks.VMDeath = &callbackVMDeath;
	callbacks.VMStart = &cbVMStart;
	callbacks.ObjectFree = &cbObjectFree;
	callbacks.GarbageCollectionFinish = &gc_finish;

	error = jvmti->SetEventCallbacks(&callbacks, (jint) sizeof(callbacks));
	check_jvmti_error(jvmti, error, "Cannot set jvmti callbacks");

	//Register for events
	error = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VM_START,
			(jthread) NULL);
	check_jvmti_error(jvmti, error, "Cannot set event notification");
	error = jvmti->SetEventNotificationMode(JVMTI_ENABLE,
			JVMTI_EVENT_GARBAGE_COLLECTION_FINISH, (jthread) NULL);
	check_jvmti_error(jvmti, error, "Cannot set event notification");
	error = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VM_INIT,
			(jthread) NULL);
	check_jvmti_error(jvmti, error, "Cannot set event notification");
	error = jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VM_DEATH,
			(jthread) NULL);
	check_jvmti_error(jvmti, error, "Cannot set event notification");
	error = jvmti->SetEventNotificationMode(JVMTI_ENABLE,
			JVMTI_EVENT_OBJECT_FREE, (jthread) NULL);
	check_jvmti_error(jvmti, error, "Cannot set event notification");


	//Set up a few locks
	error = jvmti->CreateRawMonitor("agent data", &(gdata->lock));
	check_jvmti_error(jvmti, error, "Cannot create raw monitor");

	error = jvmti->CreateRawMonitor("agent gc lock", &(gcLock));
	check_jvmti_error(jvmti, error, "Cannot create raw monitor");

	error = jvmti->CreateRawMonitor("agent gc queue", &(deleteQueueLock));
	check_jvmti_error(jvmti, error, "Cannot create raw monitor");

	return JNI_OK;
}
