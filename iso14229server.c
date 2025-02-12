/**
 * @file iso14229server.c
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include "iso14229.h"
#include "iso14229server.h"
#include "isotp-c/isotp.h"
#include "isotp-c/isotp_defines.h"

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef MIN
#define MIN(a, b) ((a) > (b) ? (b) : (a))
#endif

// ========================================================================
//                              Private Functions
// ========================================================================

/**
 * @brief Convenience function to send negative response
 *
 * @param self
 * @param response_code
 */
static inline enum Iso14229ResponseCode NegativeResponse(Iso14229ServerRequestContext *ctx,
                                                         enum Iso14229ResponseCode response_code) {
    struct Iso14229NegativeResponse *resp = (struct Iso14229NegativeResponse *)ctx->resp.buf;

    resp->negResponseSid = 0x7F;
    resp->requestSid = ctx->req.buf[0];
    resp->responseCode = response_code;
    ctx->resp.len = ISO14229_NEG_RESP_LEN;
    return response_code;
}

static inline void NoResponse(Iso14229ServerRequestContext *ctx) { ctx->resp.len = 0; }

/**
 * @brief 0x10 DiagnosticSessionControl
 *
 * @param self
 * @param data
 * @param size
 */
static enum Iso14229ResponseCode _0x10_DiagnosticSessionControl(Iso14229Server *self,
                                                                Iso14229ServerRequestContext *ctx) {
    if (ctx->req.len < 1) {
        return NegativeResponse(ctx, kIncorrectMessageLengthOrInvalidFormat);
    }

    if (NULL == self->userDiagnosticSessionControlHandler) {
        return NegativeResponse(ctx, kServiceNotSupported);
    }

    uint8_t diagSessionType = ctx->req.buf[1] & 0x4F;

    enum Iso14229ResponseCode err =
        self->userDiagnosticSessionControlHandler(&self->status, diagSessionType);

    if (kPositiveResponse != err) {
        return NegativeResponse(ctx, err);
    }

    switch (diagSessionType) {
    case kDefaultSession:
        break;
    case kProgrammingSession:
    case kExtendedDiagnostic:
    default:
        self->s3_session_timeout_timer = self->userGetms() + self->s3_ms;
        break;
    }

    self->status.sessionType = diagSessionType;

    ctx->resp.buf[0] = ISO14229_RESPONSE_SID_OF(kSID_DIAGNOSTIC_SESSION_CONTROL);
    ctx->resp.buf[1] = diagSessionType;

    // ISO14229-1-2013: Table 29
    // resolution: 1ms
    ctx->resp.buf[2] = self->p2_ms >> 8;
    ctx->resp.buf[3] = self->p2_ms;

    // resolution: 10ms
    ctx->resp.buf[4] = (self->p2_star_ms / 10) >> 8;
    ctx->resp.buf[5] = self->p2_star_ms / 10;

    ctx->resp.len = ISO14229_0X10_RESP_LEN;
    return kPositiveResponse;
}

/**
 * @brief 0x11 ECUReset
 *
 * @param self
 * @param data
 * @param size
 */
static enum Iso14229ResponseCode _0x11_ECUReset(Iso14229Server *self,
                                                Iso14229ServerRequestContext *ctx) {
    uint8_t resetType = ctx->req.buf[1] & 0x3F;
    uint8_t powerDownTime = 0xFF;

    if (ctx->req.len < ISO14229_0X11_REQ_MIN_LEN) {
        return NegativeResponse(ctx, kIncorrectMessageLengthOrInvalidFormat);
    }

    if (NULL == self->userECUResetHandler) {
        return NegativeResponse(ctx, kGeneralProgrammingFailure);
    }

    enum Iso14229ResponseCode err =
        self->userECUResetHandler(&self->status, resetType, &powerDownTime);
    if (kPositiveResponse == err) {
        self->notReadyToReceive = true;
        self->ecuResetScheduled = true;
    } else {
        return NegativeResponse(ctx, err);
    }

    ctx->resp.buf[0] = ISO14229_RESPONSE_SID_OF(kSID_ECU_RESET);
    ctx->resp.buf[1] = resetType;

    if (kEnableRapidPowerShutDown == resetType) {
        ctx->resp.buf[2] = powerDownTime;
        ctx->resp.len = ISO14229_0X11_RESP_BASE_LEN + 1;
    } else {
        ctx->resp.len = ISO14229_0X11_RESP_BASE_LEN;
    }
    return kPositiveResponse;
}

/**
 * @brief 0x22 ReadDataByIdentifier
 * @addtogroup readDataByIdentifier_0x22
 * @param self
 * @param data
 * @param size
 */
static enum Iso14229ResponseCode _0x22_ReadDataByIdentifier(Iso14229Server *self,
                                                            Iso14229ServerRequestContext *ctx) {
    uint8_t numDIDs;
    const uint8_t *data_location = NULL;
    uint16_t dataRecordSize = 0;
    uint16_t responseLength = 1;
    uint16_t dataId = 0;
    enum Iso14229ResponseCode rdbi_response;

    if (NULL == self->userRDBIHandler) {
        return NegativeResponse(ctx, kServiceNotSupported);
    }

    if (0 != (ctx->req.len - 1) % sizeof(uint16_t)) {
        return NegativeResponse(ctx, kIncorrectMessageLengthOrInvalidFormat);
    }

    numDIDs = ctx->req.len / sizeof(uint16_t);

    if (0 == numDIDs) {
        return NegativeResponse(ctx, kIncorrectMessageLengthOrInvalidFormat);
    }

    for (int did = 0; did < numDIDs; did++) {
        uint16_t idx = 1 + did * 2;
        dataId = (ctx->req.buf[idx] << 8) + ctx->req.buf[idx + 1];
        rdbi_response =
            self->userRDBIHandler(&self->status, dataId, &data_location, &dataRecordSize);

        if (kPositiveResponse == rdbi_response) {
            // TODO: make this safe: ensure that the offset doesn't exceed the
            // response buffer
            uint8_t *copylocation = ctx->resp.buf + responseLength;
            copylocation[0] = dataId >> 8;
            copylocation[1] = dataId;
            memmove(copylocation + sizeof(uint16_t), data_location, dataRecordSize);
            responseLength += sizeof(uint16_t) + dataRecordSize;
        } else {
            return NegativeResponse(ctx, rdbi_response);
        }
    }

    ctx->resp.buf[0] = ISO14229_RESPONSE_SID_OF(kSID_READ_DATA_BY_IDENTIFIER);
    ctx->resp.len = responseLength;
    return kPositiveResponse;
}

/**
 * @brief 0x27 SecurityAccess
 * @addtogroup securityAccess_0x27
 * @param self
 * @param ctx
 */
static enum Iso14229ResponseCode _0x27_SecurityAccess(Iso14229Server *self,
                                                      Iso14229ServerRequestContext *ctx) {
    uint8_t subFunction = ctx->req.buf[1];
    uint8_t response = kPositiveResponse;
    uint16_t seedLength = 0;

    if (Iso14229SecurityAccessLevelIsReserved(subFunction)) {
        return NegativeResponse(ctx, kIncorrectMessageLengthOrInvalidFormat);
    }

    if (NULL == self->userSecurityAccessGenerateSeed ||
        NULL == self->userSecurityAccessValidateKey) {
        return NegativeResponse(ctx, kServiceNotSupported);
    }

    ctx->resp.buf[0] = ISO14229_RESPONSE_SID_OF(kSID_SECURITY_ACCESS);
    ctx->resp.buf[1] = subFunction;

    // Even: sendKey
    if (0 == subFunction % 2) {
        response = self->userSecurityAccessValidateKey(&self->status, subFunction,
                                                       &ctx->req.buf[ISO14229_0X27_REQ_BASE_LEN],
                                                       ctx->req.len - ISO14229_0X27_REQ_BASE_LEN);

        if (kPositiveResponse != response) {
            return NegativeResponse(ctx, response);
        }
        self->status.securityLevel = subFunction - 1;
        ctx->resp.len = ISO14229_0X27_RESP_BASE_LEN;
        return kPositiveResponse;
    }

    // Odd: requestSeed
    else {
        /* If a server supports security, but the requested security level is already unlocked when
        a SecurityAccess ‘requestSeed’ message is received, that server shall respond with a
        SecurityAccess ‘requestSeed’ positive response message service with a seed value equal to
        zero (0). The server shall never send an all zero seed for a given security level that is
        currently locked. The client shall use this method to determine if a server is locked for a
        particular security level by checking for a non-zero seed.
        */

        uint16_t buffer_size_remaining = ctx->resp.buffer_size - ISO14229_0X27_RESP_BASE_LEN;

        response = self->userSecurityAccessGenerateSeed(
            &self->status, subFunction, &ctx->req.buf[ISO14229_0X27_REQ_BASE_LEN],
            ctx->req.len - ISO14229_0X27_REQ_BASE_LEN, &ctx->resp.buf[ISO14229_0X27_RESP_BASE_LEN],
            buffer_size_remaining, &seedLength);

        if (seedLength == 0 || seedLength > buffer_size_remaining) {
            return NegativeResponse(ctx, kGeneralProgrammingFailure);
        }

        if (kPositiveResponse != response) {
            return NegativeResponse(ctx, response);
        }
        ctx->resp.len = ISO14229_0X27_RESP_BASE_LEN + seedLength;
        return kPositiveResponse;
    }
    return NegativeResponse(ctx, kGeneralProgrammingFailure);
}

/**
 * @brief 0x28 CommunicationControl
 *
 * @addtogroup communicationControl_0x28
 * @param self
 * @param data
 * @param size
 */
static enum Iso14229ResponseCode _0x28_CommunicationControl(Iso14229Server *self,
                                                            Iso14229ServerRequestContext *ctx) {
    uint8_t controlType = ctx->req.buf[1];
    uint8_t communicationType = ctx->req.buf[2];

    if (ctx->req.len < ISO14229_0X28_REQ_BASE_LEN) {
        return NegativeResponse(ctx, kIncorrectMessageLengthOrInvalidFormat);
    }

    if (NULL == self->userCommunicationControlHandler) {
        return NegativeResponse(ctx, kServiceNotSupported);
    }

    enum Iso14229ResponseCode err =
        self->userCommunicationControlHandler(&self->status, controlType, communicationType);
    if (kPositiveResponse != err) {
        return NegativeResponse(ctx, err);
    }

    ctx->resp.buf[0] = ISO14229_RESPONSE_SID_OF(kSID_COMMUNICATION_CONTROL);
    ctx->resp.buf[1] = controlType;
    ctx->resp.len = ISO14229_0X28_RESP_LEN;
    return kPositiveResponse;
}

/**
 * @brief 0x2E WriteDataByIdentifier
 *
 * @param self
 * @param data
 * @param size
 */
static enum Iso14229ResponseCode _0x2E_WriteDataByIdentifier(Iso14229Server *self,
                                                             Iso14229ServerRequestContext *ctx) {
    uint16_t dataLen = 0;
    uint16_t dataId = 0;
    enum Iso14229ResponseCode wdbi_response;

    /* ISO14229-1 2013 Figure 21 Key 1 */
    if (ctx->req.len < ISO14229_0X2E_REQ_MIN_LEN) {
        return NegativeResponse(ctx, kIncorrectMessageLengthOrInvalidFormat);
    }

    dataId = (ctx->req.buf[1] << 8) + ctx->req.buf[2];
    dataLen = ctx->req.len - ISO14229_0X2E_REQ_BASE_LEN;

    if (NULL != self->userWDBIHandler) {
        wdbi_response = self->userWDBIHandler(&self->status, dataId,
                                              &ctx->req.buf[ISO14229_0X2E_REQ_BASE_LEN], dataLen);
        if (kPositiveResponse != wdbi_response) {
            return NegativeResponse(ctx, wdbi_response);
        }
    } else {
        return NegativeResponse(ctx, kServiceNotSupported);
    }

    ctx->resp.buf[0] = ISO14229_RESPONSE_SID_OF(kSID_WRITE_DATA_BY_IDENTIFIER);
    ctx->resp.buf[1] = dataId >> 8;
    ctx->resp.buf[2] = dataId;
    ctx->resp.len = ISO14229_0X2E_RESP_LEN;
    return kPositiveResponse;
}

/**
 * @brief 0x31 RoutineControl
 *
 * @addtogroup routineControl_0x31
 * @param self
 * @param data
 * @param size
 */
static enum Iso14229ResponseCode _0x31_RoutineControl(Iso14229Server *self,
                                                      Iso14229ServerRequestContext *ctx) {
    enum Iso14229ResponseCode err = kPositiveResponse;
    if (ctx->req.len < ISO14229_0X31_REQ_MIN_LEN) {
        return NegativeResponse(ctx, kIncorrectMessageLengthOrInvalidFormat);
    }
    if (NULL == self->userRoutineControlHandler) {
        return NegativeResponse(ctx, kServiceNotSupported);
    }
    uint8_t routineControlType = ctx->req.buf[1];
    uint16_t routineIdentifier = (ctx->req.buf[2] << 8) + ctx->req.buf[3];
    const uint8_t *optionRecord = &ctx->req.buf[ISO14229_0X31_REQ_MIN_LEN];
    uint16_t optionRecordLength = ctx->req.len - ISO14229_0X31_REQ_MIN_LEN;
    uint16_t statusRecordBufferSize = ctx->resp.buffer_size - ISO14229_0X31_RESP_MIN_LEN;
    uint16_t statusRecordLength = 0; // The actual statusRecord length written by the routine

    Iso14229RoutineControlArgs args = {
        .optionRecord = optionRecord,
        .optionRecordLength = optionRecordLength,
        .statusRecord = &ctx->resp.buf[ISO14229_0X31_RESP_MIN_LEN],
        .statusRecordBufferSize = statusRecordBufferSize,
        .statusRecordLength = &statusRecordLength,
    };

    switch (routineControlType) {
    case kStartRoutine:
    case kStopRoutine:
    case kRequestRoutineResults:
        err = self->userRoutineControlHandler(&self->status, routineControlType, routineIdentifier,
                                              &args);
        if (kPositiveResponse != err) {
            return NegativeResponse(ctx, err);
        }
        break;
    default:
        return NegativeResponse(ctx, kIncorrectMessageLengthOrInvalidFormat);
    }

    if (statusRecordLength > args.statusRecordBufferSize) {
        return NegativeResponse(ctx, kGeneralProgrammingFailure);
    }

    ctx->resp.buf[0] = ISO14229_RESPONSE_SID_OF(kSID_ROUTINE_CONTROL);
    ctx->resp.buf[1] = routineControlType;
    ctx->resp.buf[2] = routineIdentifier >> 8;
    ctx->resp.buf[3] = routineIdentifier;
    ctx->resp.len = ISO14229_0X31_RESP_MIN_LEN + statusRecordLength;
    return kPositiveResponse;
}

/**
 * @brief 0x34 RequestDownload
 *
 * @param self
 * @param data
 * @param size
 */
static enum Iso14229ResponseCode _0x34_RequestDownload(Iso14229Server *self,
                                                       Iso14229ServerRequestContext *ctx) {
    enum Iso14229ResponseCode err;
    uint16_t maxNumberOfBlockLength = 0;
    size_t memoryAddress = 0;
    size_t memorySize = 0;

    if (NULL == self->userRequestDownloadHandler) {
        return NegativeResponse(ctx, kServiceNotSupported);
    }

    if (NULL != self->downloadHandler) {
        return NegativeResponse(ctx, kConditionsNotCorrect);
    }

    if (ctx->req.len < ISO14229_0X34_REQ_BASE_LEN) {
        return NegativeResponse(ctx, kIncorrectMessageLengthOrInvalidFormat);
    }

    uint8_t dataFormatIdentifier = ctx->req.buf[1];
    uint8_t memorySizeLength = (ctx->req.buf[2] & 0xF0) >> 4;
    uint8_t memoryAddressLength = ctx->req.buf[2] & 0x0F;

    if (memorySizeLength == 0 || memorySizeLength > sizeof(memorySize)) {
        return NegativeResponse(ctx, kRequestOutOfRange);
    }

    if (memoryAddressLength == 0 || memoryAddressLength > sizeof(memoryAddress)) {
        return NegativeResponse(ctx, kRequestOutOfRange);
    }

    if (ctx->req.len < ISO14229_0X34_REQ_BASE_LEN + memorySizeLength + memoryAddressLength) {
        return NegativeResponse(ctx, kIncorrectMessageLengthOrInvalidFormat);
    }

    for (int byteIdx = 0; byteIdx < memoryAddressLength; byteIdx++) {
        uint8_t byte = ctx->req.buf[ISO14229_0X34_REQ_BASE_LEN + byteIdx];
        uint8_t shiftBytes = memoryAddressLength - 1 - byteIdx;
        memoryAddress |= byte << (8 * shiftBytes);
    }

    for (int byteIdx = 0; byteIdx < memorySizeLength; byteIdx++) {
        uint8_t byte = ctx->req.buf[ISO14229_0X34_REQ_BASE_LEN + memoryAddressLength + byteIdx];
        uint8_t shiftBytes = memorySizeLength - 1 - byteIdx;
        memorySize |= byte << (8 * shiftBytes);
    }

    assert(self->userRequestDownloadHandler);
    assert(NULL == self->downloadHandler);
    err = self->userRequestDownloadHandler(&self->status, (void *)memoryAddress, memorySize,
                                           dataFormatIdentifier, &self->downloadHandler,
                                           &maxNumberOfBlockLength);

    if (kPositiveResponse != err) {
        self->downloadHandler = NULL;
        return NegativeResponse(ctx, err);
    } else {
        if (NULL == self->downloadHandler) {
            ISO14229USERDEBUG("ERROR: handler must not be NULL!");
            return NegativeResponse(ctx, kGeneralProgrammingFailure);
        }
        if (NULL == self->downloadHandler->onTransfer || NULL == self->downloadHandler->onExit) {
            ISO14229USERDEBUG("ERROR: onTransfer and onExit must be implemented!");
            return NegativeResponse(ctx, kGeneralProgrammingFailure);
        }
    }

    if (maxNumberOfBlockLength < 3) {
        ISO14229USERDEBUG("ERROR: maxNumberOfBlockLength too short");
        return NegativeResponse(ctx, kGeneralProgrammingFailure);
    }

    Iso14229DownloadHandlerInit(self->downloadHandler, memorySize);

    // ISO-14229-1:2013 Table 401:
    uint8_t lengthFormatIdentifier = sizeof(maxNumberOfBlockLength) << 4;

// ISO-15764-2-2004 section 5.3.3
#define ISOTP_MTU 4095UL

/* ISO-14229-1:2013 Table 396: maxNumberOfBlockLength
This parameter is used by the requestDownload positive response message to
inform the client how many data bytes (maxNumberOfBlockLength) to include in
each TransferData request message from the client. This length reflects the
complete message length, including the service identifier and the
data-parameters present in the TransferData request message.
*/
#define MAX_TRANSFER_DATA_PAYLOAD_LEN (ISOTP_MTU)

    maxNumberOfBlockLength = MIN(maxNumberOfBlockLength, MAX_TRANSFER_DATA_PAYLOAD_LEN);

    ctx->resp.buf[0] = ISO14229_RESPONSE_SID_OF(kSID_REQUEST_DOWNLOAD);
    ctx->resp.buf[1] = lengthFormatIdentifier;
    for (uint8_t idx = 0; idx < sizeof(maxNumberOfBlockLength); idx++) {
        uint8_t shiftBytes = sizeof(maxNumberOfBlockLength) - 1 - idx;
        uint8_t byte = maxNumberOfBlockLength >> (shiftBytes * 8);
        ctx->resp.buf[ISO14229_0X34_RESP_BASE_LEN + idx] = byte;
    }
    ctx->resp.len = ISO14229_0X34_RESP_BASE_LEN + sizeof(maxNumberOfBlockLength);
    return kPositiveResponse;
}

/**
 * @brief 0x36 TransferData
 * @addtogroup transferData_0x36
 * @param self
 * @param data
 * @param size
 */
static enum Iso14229ResponseCode _0x36_TransferData(Iso14229Server *self,
                                                    Iso14229ServerRequestContext *ctx) {
    enum Iso14229ResponseCode err;
    uint16_t request_data_len = ctx->req.len - ISO14229_0X36_REQ_BASE_LEN;

    if (ctx->req.len < ISO14229_0X36_REQ_BASE_LEN) {
        err = kIncorrectMessageLengthOrInvalidFormat;
        goto fail;
    }

    uint8_t blockSequenceCounter = ctx->req.buf[1];

    if (NULL == self->downloadHandler) {
        return NegativeResponse(ctx, kUploadDownloadNotAccepted);
    }

    assert(self->downloadHandler);

    if (!self->status.RCRRP) {
        if (blockSequenceCounter != self->downloadHandler->blockSequenceCounter) {
            err = kRequestSequenceError;
            goto fail;
        } else {
            self->downloadHandler->blockSequenceCounter++;
        }
    }

    if (self->downloadHandler->numBytesTransferred + request_data_len >
        self->downloadHandler->requestedTransferSize) {
        err = kTransferDataSuspended;
        goto fail;
    }

    err = self->downloadHandler->onTransfer(&self->status, self->downloadHandler->userCtx,
                                            &ctx->req.buf[ISO14229_0X36_REQ_BASE_LEN],
                                            request_data_len);

    switch (err) {
    case kPositiveResponse:
        self->downloadHandler->numBytesTransferred += request_data_len;
        ctx->resp.buf[0] = ISO14229_RESPONSE_SID_OF(kSID_TRANSFER_DATA);
        ctx->resp.buf[1] = blockSequenceCounter;
        ctx->resp.len = ISO14229_0X36_RESP_BASE_LEN; // TODO: 加transferResponseParameterRecord
        return kPositiveResponse;
    case kRequestCorrectlyReceived_ResponsePending:
        return NegativeResponse(ctx, kRequestCorrectlyReceived_ResponsePending);
    default:
        goto fail;
    }

fail:
    self->downloadHandler = NULL;
    return NegativeResponse(ctx, err);
}

/**
 * @brief 0x37 RequestTransferExit
 * @addtogroup requestTransferExit_0x37
 * @param self
 * @param data
 * @param size
 */
static enum Iso14229ResponseCode _0x37_RequestTransferExit(Iso14229Server *self,
                                                           Iso14229ServerRequestContext *ctx) {
    enum Iso14229ResponseCode err;

    if (NULL == self->downloadHandler) {
        return NegativeResponse(ctx, kUploadDownloadNotAccepted);
    }

    assert(self->downloadHandler);

    uint16_t buffer_size = ctx->resp.buffer_size - ISO14229_0X37_RESP_BASE_LEN;
    uint16_t transferResponseParameterRecordSize = 0;

    err = self->downloadHandler->onExit(&self->status, self->downloadHandler->userCtx, buffer_size,
                                        &ctx->resp.buf[ISO14229_0X37_RESP_BASE_LEN],
                                        &transferResponseParameterRecordSize);

    if (err != kPositiveResponse) {
        return NegativeResponse(ctx, err);
    }

    if (transferResponseParameterRecordSize > buffer_size) {
        return NegativeResponse(ctx, kGeneralProgrammingFailure);
    }

    self->downloadHandler = NULL;
    ctx->resp.buf[0] = ISO14229_RESPONSE_SID_OF(kSID_REQUEST_TRANSFER_EXIT);
    ctx->resp.len = ISO14229_0X37_RESP_BASE_LEN + transferResponseParameterRecordSize;
    return kPositiveResponse;
}

/**
 * @brief 0x3E TesterPresent
 *
 * @param self
 * @param data
 * @param size
 */
static enum Iso14229ResponseCode _0x3E_TesterPresent(Iso14229Server *self,
                                                     Iso14229ServerRequestContext *ctx) {
    if (ctx->req.len < ISO14229_0X3E_REQ_MIN_LEN) {
        return NegativeResponse(ctx, kIncorrectMessageLengthOrInvalidFormat);
    }
    self->s3_session_timeout_timer = self->userGetms() + self->s3_ms;
    uint8_t zeroSubFunction = ctx->req.buf[1];
    ctx->resp.buf[0] = ISO14229_RESPONSE_SID_OF(kSID_TESTER_PRESENT);
    ctx->resp.buf[1] = zeroSubFunction & 0x3F;
    ctx->resp.len = ISO14229_0X3E_RESP_LEN;
    return kPositiveResponse;
}

/**
 * @brief 0x85 ControlDtcSetting
 *
 * @param self
 * @param data
 * @param size
 */
static enum Iso14229ResponseCode _0x85_ControlDTCSetting(Iso14229Server *self,
                                                         Iso14229ServerRequestContext *ctx) {
    (void)self;
    if (ctx->req.len < ISO14229_0X85_REQ_BASE_LEN) {
        return NegativeResponse(ctx, kIncorrectMessageLengthOrInvalidFormat);
    }
    uint8_t dtcSettingType = ctx->req.buf[1] & 0x3F;

    ctx->resp.buf[0] = ISO14229_RESPONSE_SID_OF(kSID_CONTROL_DTC_SETTING);
    ctx->resp.buf[1] = dtcSettingType;
    ctx->resp.len = ISO14229_0X85_RESP_LEN;
    return kPositiveResponse;
}

/**
 * @brief Call the service if it exists, modifying the response if the spec calls for it.
 * @note see ISO14229-1 2013 7.5.5 Pseudo code example of server response behavior
 *
 * @param self
 * @param service NULL if the service is not supported
 * @param ctx
 */
static enum Iso14229ResponseCode evaluateServiceResponse(Iso14229Server *self,
                                                         Iso14229Service service,
                                                         Iso14229ServerRequestContext *ctx) {
    enum Iso14229ResponseCode response = kPositiveResponse;
    bool suppressResponse = false;
    uint8_t sid = ctx->req.buf[0];

    if (NULL == service) {
        return NegativeResponse(ctx, kServiceNotSupported);
    }

    switch (sid) {
    /* CASE Service_with_sub-function */
    /* test if service with sub-function is supported */
    case kSID_DIAGNOSTIC_SESSION_CONTROL:
    case kSID_ECU_RESET:
    case kSID_READ_DTC_INFORMATION:
    case kSID_SECURITY_ACCESS:
    case kSID_COMMUNICATION_CONTROL:
    case kSID_ROUTINE_CONTROL:
    case kSID_TESTER_PRESENT:
    case kSID_ACCESS_TIMING_PARAMETER:
    case kSID_SECURED_DATA_TRANSMISSION:
    case kSID_CONTROL_DTC_SETTING:
    case kSID_RESPONSE_ON_EVENT:

        /* check minimum length of message with sub-function */
        if (ctx->req.len >= 2) {
            /* get sub-function parameter value without bit 7 */
            // switch (ctx->req.as.raw[0] & 0x7F) {

            // }
            // Let the service callback determine whether or not the sub-function parameter value is
            // supported
            response = service(self, ctx);
        } else {
            /* NRC 0x13: incorrectMessageLengthOrInvalidFormat */
            response = kIncorrectMessageLengthOrInvalidFormat;
        }

        bool suppressPosRspMsgIndicationBit = ctx->req.buf[1] & 0x80;

        /* test if positive response is required and if responseCode is positive 0x00 */
        if ((suppressPosRspMsgIndicationBit) && (response == kPositiveResponse) &&
            (
                // TODO: *not yet a NRC 0x78 response sent*
                true)) {
            suppressResponse = true;
        } else {
            suppressResponse = false;
        }
        break;

    /* CASE Service_without_sub-function */
    /* test if service without sub-function is supported */
    case kSID_READ_DATA_BY_IDENTIFIER:
    case kSID_READ_MEMORY_BY_ADDRESS:
    case kSID_READ_SCALING_DATA_BY_IDENTIFIER:
    case kSID_READ_PERIODIC_DATA_BY_IDENTIFIER:
    case kSID_DYNAMICALLY_DEFINE_DATA_IDENTIFIER:
    case kSID_WRITE_DATA_BY_IDENTIFIER:
    case kSID_REQUEST_DOWNLOAD:
    case kSID_REQUEST_UPLOAD:
    case kSID_TRANSFER_DATA:
    case kSID_REQUEST_TRANSFER_EXIT:
    case kSID_REQUEST_FILE_TRANSFER:
    case kSID_WRITE_MEMORY_BY_ADDRESS:
    case kSID_CLEAR_DIAGNOSTIC_INFORMATION:
    case kSID_INPUT_CONTROL_BY_IDENTIFIER:
        response = service(self, ctx);
        break;

    default:
        response = kServiceNotSupported;
        break;
    }

    if ((kAddressingSchemeFunctional == ctx->req.addressingScheme) &&
        ((kServiceNotSupported == response) || (kSubFunctionNotSupported == response) ||
         (kServiceNotSupportedInActiveSession == response) ||
         (kSubFunctionNotSupportedInActiveSession == response) ||
         (kRequestOutOfRange == response)) &&
        (
            // TODO: *not yet a NRC 0x78 response sent*
            true)) {
        suppressResponse = true; /* Suppress negative response message */
        NoResponse(ctx);
    } else {
        if (suppressResponse) { /* Suppress positive response message */
            NoResponse(ctx);
        } else { /* send negative or positive response */
            ;
        }
    }
    return response;
}

static Iso14229Service getServiceForSID(enum Iso14229DiagnosticServiceId sid) {
#define X(str_ident, sid, func)                                                                    \
    case sid:                                                                                      \
        return func;
    switch (sid) {
        ISO14229_SID_LIST
#undef X
    default:
        ISO14229USERDEBUG("no handler for request SID %x.\n", sid);
        return NULL;
    }
}

/**
 * @brief Call the service matching the requested SID
 *
 * @param self
 * @param buf   incoming data from ISO-TP layer
 * @param size  size of buf
 */
void iso14229ProcessUDSLayer(Iso14229Server *self, IsoTpLink *link,
                             enum Iso14229AddressingScheme addressingScheme) {
    uint8_t sid = link->receive_buffer[0];
    Iso14229Service handler = getServiceForSID(sid);
    Iso14229ServerRequestContext ctx = {
        .req =
            {
                .buf = link->receive_buffer,
                .len = link->receive_size,
                .addressingScheme = addressingScheme,
            },
        .resp = {.buf = link->send_buffer, .len = 0, .buffer_size = link->send_buf_size},
    };

    enum Iso14229ResponseCode response = evaluateServiceResponse(self, handler, &ctx);

    if (kRequestCorrectlyReceived_ResponsePending == response) {
        self->status.RCRRP = true;
        self->notReadyToReceive = true;
    } else {
        self->status.RCRRP = false;
    }

    if (ctx.resp.len) {
        isotp_send(link, ctx.resp.buf, ctx.resp.len);
    }
}

// ========================================================================
//                             Public Functions
// ========================================================================

/**
 * @brief \~chinese 初始化服务器 \~english Initialize the server
 *
 * @param self
 * @param cfg
 * @return int
 */
void Iso14229ServerInit(Iso14229Server *self, const Iso14229ServerConfig *cfg) {
    assert(self);
    assert(cfg);
    assert(cfg->phys_link);
    assert(cfg->func_link);
    assert(cfg->phys_link_send_buffer);
    assert(cfg->phys_link_send_buf_size > 2);
    assert(cfg->phys_link_receive_buffer);
    assert(cfg->phys_link_recv_buf_size > 2);
    assert(cfg->func_link_send_buffer);
    assert(cfg->func_link_send_buf_size > 2);
    assert(cfg->func_link_receive_buffer);
    assert(cfg->func_link_recv_buf_size > 2);
    assert(cfg->userGetms);
    assert(cfg->userSessionTimeoutCallback);
    assert(cfg->userCANTransmit);
    assert(cfg->userCANRxPoll);

    isotp_init_link(cfg->phys_link, cfg->send_id, cfg->phys_link_send_buffer,
                    cfg->phys_link_send_buf_size, cfg->phys_link_receive_buffer,
                    cfg->phys_link_recv_buf_size, cfg->userGetms, cfg->userCANTransmit,
                    cfg->userDebug);

    isotp_init_link(cfg->func_link, cfg->send_id, cfg->func_link_send_buffer,
                    cfg->func_link_send_buf_size, cfg->func_link_receive_buffer,
                    cfg->func_link_recv_buf_size, cfg->userGetms, cfg->userCANTransmit,
                    cfg->userDebug);

    memset(self, 0, sizeof(Iso14229Server));

    self->phys_link = cfg->phys_link;
    self->func_link = cfg->func_link;
    self->phys_recv_id = cfg->phys_recv_id;
    self->func_recv_id = cfg->func_recv_id;
    self->p2_ms = cfg->p2_ms;
    self->p2_star_ms = cfg->p2_star_ms;
    self->s3_ms = cfg->s3_ms;
    self->userSessionTimeoutCallback = cfg->userSessionTimeoutCallback;
    self->userGetms = cfg->userGetms;
    self->userCANTransmit = cfg->userCANTransmit;
    self->userCANRxPoll = cfg->userCANRxPoll;
    self->userDebug = cfg->userDebug;
    self->userDiagnosticSessionControlHandler = cfg->userDiagnosticSessionControlHandler;
    self->userECUResetHandler = cfg->userECUResetHandler;
    self->userRDBIHandler = cfg->userRDBIHandler;
    self->userWDBIHandler = cfg->userWDBIHandler;
    self->userCommunicationControlHandler = cfg->userCommunicationControlHandler;
    self->userSecurityAccessGenerateSeed = cfg->userSecurityAccessGenerateSeed;
    self->userSecurityAccessValidateKey = cfg->userSecurityAccessValidateKey;
    self->userRoutineControlHandler = cfg->userRoutineControlHandler;
    self->userRequestDownloadHandler = cfg->userRequestDownloadHandler;

    self->status.sessionType = kDefaultSession;

    // Initialize p2_timer to an already past time, otherwise the server's
    // response to incoming messages will be delayed.
    self->p2_timer = self->userGetms() - self->p2_ms;

    // Set the session timeout for s3 milliseconds from now.
    self->s3_session_timeout_timer = self->userGetms() + self->s3_ms;
}

static void _ProcessLinks(Iso14229Server *self) {
    int recv_status = ISOTP_RET_NO_DATA;

    // If the user service handler responded RCRRP and the send link is now idle,
    // the response has been sent and the long-running service can now be called.
    if (self->status.RCRRP && ISOTP_SEND_STATUS_IDLE == self->phys_link->send_status) {
        iso14229ProcessUDSLayer(self, self->phys_link, kAddressingSchemePhysical);
        self->notReadyToReceive = self->status.RCRRP;
        return;
    }

    if (self->notReadyToReceive) {
        return;
    }

    // new data may be processed only after p2 has elapsed
    if (Iso14229TimeAfter(self->userGetms(), self->p2_timer)) {

        // priority goes to the physical link
        recv_status =
            isotp_receive(self->phys_link, self->phys_link->receive_buffer,
                          self->phys_link->receive_buf_size, &self->phys_link->receive_size);
        switch (recv_status) {
        case ISOTP_RET_OK:
            iso14229ProcessUDSLayer(self, self->phys_link, kAddressingSchemePhysical);
            self->p2_timer = self->userGetms() + self->p2_ms;
            return;
            break;
        case ISOTP_RET_NO_DATA:
            break;
        default:
            assert(0);
            break;
        }

        recv_status =
            isotp_receive(self->func_link, self->func_link->receive_buffer,
                          self->func_link->receive_buf_size, &self->func_link->receive_size);

        switch (recv_status) {
        case ISOTP_RET_OK:
            iso14229ProcessUDSLayer(self, self->func_link, kAddressingSchemeFunctional);
            self->p2_timer = self->userGetms() + self->p2_ms;
            return;
            break;
        case ISOTP_RET_NO_DATA:
            break;
        default:
            assert(0);
            break;
        }
    }
}

void Iso14229ServerPoll(Iso14229Server *self) {
    uint32_t arb_id;
    uint8_t data[8];
    uint8_t size;

    if (kCANRxSome == self->userCANRxPoll(&arb_id, data, &size)) {
        if (arb_id == self->phys_recv_id) {
            isotp_on_can_message(self->phys_link, (uint8_t *)data, size);
        } else if (arb_id == self->func_recv_id) {
            isotp_on_can_message(self->func_link, (uint8_t *)data, size);
        }
    }

    isotp_poll(self->phys_link);
    isotp_poll(self->func_link);

    // ISO14229-1-2013 Figure 38: Session Timeout (S3)
    if (kDefaultSession != self->status.sessionType &&
        Iso14229TimeAfter(self->userGetms(), self->s3_session_timeout_timer)) {
        self->userSessionTimeoutCallback();
    }

    _ProcessLinks(self);
}
