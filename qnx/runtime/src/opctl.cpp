// opctl: operator command line. `opctl reset` asks motiond to release a latched SAFE_STOP.
// Exit code 0 = command accepted by motiond; the outcome is visible in motiond/telemetry output.
#include <stdio.h>
#include <string.h>

#include "svc_common.h"

using namespace edgert;

int main(int argc, char** argv) {
    if (argc != 2 || strcmp(argv[1], "reset") != 0) { fprintf(stderr, "usage: opctl reset\n"); return 2; }
    Client* c = connect_retry(NAME_MOTIOND, 2000);
    if (!c) { fprintf(stderr, "opctl: motiond not reachable\n"); return 1; }
    OperatorCommand cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.header = make_header(MsgType::OPERATOR_COMMAND);
    cmd.sequence = 1;
    cmd.command = static_cast<uint8_t>(OperatorCmd::RESET);
    OperatorAck ak;
    memset(&ak, 0, sizeof ak);
    int n = client_call(c, &cmd, sizeof cmd, &ak, sizeof ak, 500);
    client_close(c);
    if (n != static_cast<int>(sizeof ak) || ak.header.type != static_cast<uint16_t>(MsgType::OPERATOR_ACK)) {
        fprintf(stderr, "opctl: no valid reply\n");
        return 1;
    }
    const char* r[] = {"queued", "not latched (nothing to reset)", "rejected", "unsupported"};
    printf("opctl reset: %s (state=%u reason=%u)\n", ak.result < 4 ? r[ak.result] : "?", ak.state, ak.reason);
    return ak.result == static_cast<uint8_t>(OperatorResult::OK) ? 0 : 3;
}
