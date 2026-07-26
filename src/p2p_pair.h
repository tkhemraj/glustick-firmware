#pragma once
#ifdef TRANSPORT_P2P

#include <Arduino.h>

// Returns true if these devices have already paired and the key is in NVS.
bool p2p_pair_is_complete();

// Interactive LoRa pairing wizard. Blocks until paired or the user cancels.
//   HOST — generates a random key, displays a 6-char code, waits for joiner.
//   JOIN — user types the code shown on the host; radios handshake and save key.
// Returns true on success; saves shared key to NVS so future boots skip pairing.
bool p2p_pair_run();

// Load the saved 32-char hex key into out_hex33 (caller must supply >= 33 bytes).
void p2p_pair_load_key(char *out_hex33);

// Erase the saved pairing from NVS so the next boot runs the HOST/JOIN flow again.
void p2p_pair_clear();

#endif // TRANSPORT_P2P
