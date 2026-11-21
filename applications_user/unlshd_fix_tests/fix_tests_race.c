#include "fix_tests.h"

#include <furi.h>
#include <gps/gps.h>
#include <network/network.h>
#include <rpc/rpc_app.h>

#define RACE_ROUNDS         (200U)
#define RACE_FEED_TIMEOUT   (10U)
// A call stays in the callback this long, standing for an app using its context
#define RACE_CALL_MS        (2U)
// One frame every RACE_FEED_PERIOD_MS: calls take up about half the time. Not a flood: with a
// call always running, the session worker would take the patched service's lock straight
// back after each call, and unsubscribing, which waits for that lock, would never get it
#define RACE_FEED_PERIOD_MS (4U)

// One delimited PB_Main message each, as the companion app sends them, holding nothing but
// an empty content field: 87 is gps_location, 83 is network_state_changed. The session hands
// it to the service, which calls the app callback just as it does for a real fix or event.
static const uint8_t race_gps_frame[] = {0x03, 0xBA, 0x05, 0x00};
static const uint8_t race_network_frame[] = {0x03, 0x9A, 0x05, 0x00};

typedef struct {
    // Written by the test thread, read by the callback
    volatile uint32_t round;
    volatile uint32_t closed_round;
    // Written by the callback, on the session worker; read once the session is gone
    volatile uint32_t calls;
    volatile uint32_t late;

    volatile bool stop;
    RpcSession* session;
    FuriSemaphore* terminated;
    const uint8_t* frame;
    size_t frame_size;
} Race;

static void race_on_call(Race* race) {
    const uint32_t round = race->round;
    race->calls++;
    furi_delay_ms(RACE_CALL_MS);
    // Unsubscribing returned while this call was running, or before it started: an app that
    // frees the context at that point has this call use freed memory
    if(race->closed_round >= round) {
        race->late++;
    }
}

static void race_gps_callback(GpsStatus status, const GpsLocation* location, void* context) {
    UNUSED(status);
    UNUSED(location);
    race_on_call(context);
}

static void race_network_callback(const NetworkEvent* event, void* context) {
    UNUSED(event);
    race_on_call(context);
}

static void race_send_bytes(void* context, uint8_t* bytes, size_t bytes_len) {
    // Nobody is at the other end: replies are dropped
    UNUSED(context);
    UNUSED(bytes);
    UNUSED(bytes_len);
}

static void race_terminated(void* context) {
    Race* race = context;
    furi_semaphore_release(race->terminated);
}

static int32_t race_feeder(void* context) {
    Race* race = context;

    while(!race->stop) {
        // A frame goes in whole, or the session would lose track of where messages start.
        // Only a stop cuts one short, and the session is closed right after
        size_t sent = 0;
        while(sent < race->frame_size && !race->stop) {
            sent += rpc_session_feed(
                race->session, race->frame + sent, race->frame_size - sent, RACE_FEED_TIMEOUT);
        }
        furi_delay_ms(RACE_FEED_PERIOD_MS);
    }

    return 0;
}

static void race_subscribe(FixTestsRaceTarget target, void* service, Race* race) {
    if(target == FixTestsRaceGps) {
        gps_set_location_callback(service, race ? race_gps_callback : NULL, race);
    } else {
        network_set_event_callback(service, race ? race_network_callback : NULL, race);
    }
}

void fix_tests_race_run(FixTestsRaceTarget target, FixTestsRaceResult* result) {
    furi_check(result);

    Race* race = malloc(sizeof(Race));
    race->terminated = furi_semaphore_alloc(1, 0);
    if(target == FixTestsRaceGps) {
        race->frame = race_gps_frame;
        race->frame_size = sizeof(race_gps_frame);
    } else {
        race->frame = race_network_frame;
        race->frame_size = sizeof(race_network_frame);
    }

    const char* record = target == FixTestsRaceGps ? RECORD_GPS : RECORD_NETWORK;
    void* service = furi_record_open(record);
    Rpc* rpc = furi_record_open(RECORD_RPC);

    race->session = rpc_session_open(rpc, RpcOwnerUnknown);
    rpc_session_set_context(race->session, race);
    rpc_session_set_send_bytes_callback(race->session, race_send_bytes);
    rpc_session_set_terminated_callback(race->session, race_terminated);

    FuriThread* feeder = furi_thread_alloc_ex("FixTestsFeeder", 1024, race_feeder, race);
    furi_thread_start(feeder);

    for(uint32_t round = 1; round <= RACE_ROUNDS; round++) {
        race->round = round;
        race_subscribe(target, service, race);
        // 1 to 4 ms: unsubscribing falls at every point of the feed period in turn
        furi_delay_ms(1 + round % 4);
        race_subscribe(target, service, NULL);
        // The setter has returned: from here on, no call of this round may still be running
        race->closed_round = round;
    }

    race->stop = true;
    furi_thread_join(feeder);
    furi_thread_free(feeder);

    // Where the defect is, calls can still be in flight. What they touch (race, the
    // callbacks, this app's code) has to stay put until the session worker has finished
    rpc_session_close(race->session);
    furi_check(furi_semaphore_acquire(race->terminated, FuriWaitForever) == FuriStatusOk);
    // race_terminated() runs on another thread and has to have returned. The margin also
    // covers a late call from another session, the phone's, when it streams at the same time
    furi_delay_ms(50);

    result->rounds = RACE_ROUNDS;
    result->calls = race->calls;
    result->late = race->late;

    furi_record_close(RECORD_RPC);
    furi_record_close(record);
    furi_semaphore_free(race->terminated);
    free(race);
}
