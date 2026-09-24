"""
api/websocket.py
─────────────────
WebSocket endpoints for real-time ECG streaming.

WS /ws/ecg (UI clients)
────────────────────────
Dashboard clients connect and receive continuous JSON frames:
{
  "timestamp":   "2026-07-17T12:10:31.123456+00:00",
  "ecg":         531.42,
  "bpm":         74,
  "quality":     "Good",
  "quality_pct": 88,
  "status":      "Normal Sinus Rhythm",
  "session_id":  "S-ABC123",
  "analysis": {
    "rhythm":        "Normal Sinus Rhythm",
    "confidence":    97,
    "riskLevel":     "Low",
    "signalQuality": "Good",
    "heartRateTrend":"Stable"
  }
}

WS /ws/esp32 (Hardware)
────────────────────────
ESP32 devices send telemetry packets:
{
  "device_id":  "ESP32_DEV_1",
  "voltage":    1.65,
  "leads_off":  false
}

Authentication:
  - /ws/ecg:   JWT via query param ?token=<access_token>
  - /ws/esp32: No auth (hardware client)
"""

from __future__ import annotations

import json
from fastapi import APIRouter, Query, WebSocket, WebSocketDisconnect, status

from app.core.logger import get_logger
from app.core.security import decode_token
from app.services.ecg_service import ecg_service

router = APIRouter(tags=["WebSocket"])
logger = get_logger("api.websocket")


@router.websocket("/ws/ecg")
async def ecg_websocket(
    websocket: WebSocket,
    token: str = Query(default=None, description="JWT access token"),
):
    """
    Real-time ECG streaming WebSocket endpoint.

    Authenticates via ?token=<jwt> query parameter.
    Broadcasts ECG samples at ~25 fps (configurable via WS_BROADCAST_INTERVAL).

    Returns WS 1008 (Policy Violation) if the token is missing or invalid.
    """
    # ── Token guard ───────────────────────────────────────
    if not token:
        await websocket.accept()
        await websocket.send_json({
            "error": "Authentication required. Connect with ?token=<access_token>",
            "hint": "POST /auth/login to obtain a token."
        })
        await websocket.close(code=status.WS_1008_POLICY_VIOLATION)
        logger.warning("WebSocket rejected — no token provided")
        return

    # ── Validate token ────────────────────────────────────
    try:
        user = decode_token(token)
    except Exception:
        await websocket.accept()
        await websocket.send_json({"error": "Invalid or expired token."})
        await websocket.close(code=status.WS_1008_POLICY_VIOLATION)
        logger.warning("WebSocket rejected — invalid token")
        return

    await ecg_service.connect_ws(websocket)
    logger.info("WebSocket authenticated as '%s'", user.sub)

    try:
        # Keep connection alive; actual data is pushed by ECGService broadcast loop
        while True:
            # Accept any client messages (ping-pong / control frames)
            data = await websocket.receive_text()
            # Could handle client commands here (e.g. {"cmd": "pause"})
    except WebSocketDisconnect:
        logger.info("WebSocket client disconnected normally")
    except Exception as exc:
        logger.error("WebSocket error: %s", exc)
    finally:
        await ecg_service.disconnect_ws(websocket)


@router.websocket("/ws/esp32")
async def esp32_hardware_websocket(websocket: WebSocket):
    """
    ESP32 hardware telemetry ingestion endpoint.
    
    Accepts JSON packets:
    {
      "device_id":  "ESP32_DEV_1",
      "voltage":    1.65,
      "leads_off":  false
    }
    
    No authentication required (hardware client).
    Timeout: 3 seconds of silence triggers "Hardware Disconnected" state.
    """
    await websocket.accept()
    logger.info("[ESP32] Hardware connected")
    
    # Notify ECGService that hardware is connected
    ecg_service._hardware_connected = True
    ecg_service._last_hardware_packet_time = asyncio.get_event_loop().time()
    
    try:
        while True:
            data = await websocket.receive_text()
            try:
                packet = json.loads(data)
                device_id  = packet.get("device_id", "UNKNOWN")
                voltage    = packet.get("voltage", 0.0)
                leads_off  = packet.get("leads_off", False)
                
                # Update last packet timestamp
                ecg_service._last_hardware_packet_time = asyncio.get_event_loop().time()
                
                # Convert voltage (0–3.3V) to normalized ECG value (-1 to 1)
                # AD8232 mid-point is ~1.65V
                normalized = (voltage - 1.65) / 1.65
                
                # Feed into ECG processing pipeline (same path as mock)
                if not leads_off:
                    ecg_service._on_sample(normalized)
                else:
                    # Leads-off: send flatline (0.0) to trigger quality alerts
                    ecg_service._on_sample(0.0)
                    
            except json.JSONDecodeError:
                logger.warning("[ESP32] Invalid JSON: %s", data[:100])
            except Exception as exc:
                logger.error("[ESP32] Packet processing error: %s", exc)
                
    except WebSocketDisconnect:
        logger.info("[ESP32] Hardware disconnected normally")
    except Exception as exc:
        logger.error("[ESP32] Connection error: %s", exc)
    finally:
        ecg_service._hardware_connected = False
        logger.info("[ESP32] Hardware connection closed")


# Import asyncio for timestamp tracking
import asyncio
