#pragma once

// GET /api/sync/v2/progress/positions: where each device of the account stands in a book, the
// most recent reading first. Asked for only when the reader asks, from the drawer, after having
// answered the prompt: a focus check says nothing then, and may even be this device's own head.

#include "Events.h"
#include "JsonStream.h"

namespace borges {

// The first other device with a place to go to, in the fields a RemoteEvent has for it:
// eventId is the server's recovery_id. False if there is none, or the document does not parse.
bool parseOtherPosition(JsonScanner& scan, RemoteEvent& out);

}  // namespace borges
