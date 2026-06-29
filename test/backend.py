#!/usr/bin/env python3
"""
FastAPI test backend for RPS proxy testing.
- HTTP: echo, headers, status codes, large body, keep-alive
- WebSocket: echo with ping/pong, broadcast
- Hidden routes: for testing direct vs proxy access
Usage: pip install fastapi uvicorn && python3 backend.py [port]
"""
import sys
import asyncio
from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Request
from fastapi.responses import PlainTextResponse, JSONResponse, StreamingResponse
import uvicorn

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9090

app = FastAPI(title="RPS Test Backend")


@app.get("/")
async def root():
    return PlainTextResponse("Hello from backend")


@app.api_route("/echo", methods=["GET", "POST", "PUT", "DELETE"])
async def echo(request: Request):
    """回显完整请求信息"""
    body = await request.body()
    lines = [f"{request.method} {request.url.path} HTTP/1.1"]
    for k, v in request.headers.items():
        lines.append(f"{k}: {v}")
    lines.append("")
    lines.append(body.decode(errors="replace"))
    return PlainTextResponse("\n".join(lines))


@app.get("/headers")
async def headers(request: Request):
    """返回收到的所有请求头"""
    result = []
    for k, v in request.headers.items():
        result.append(f"{k}: {v}")
    return PlainTextResponse("\n".join(result))


@app.get("/ip")
async def show_ip(request: Request):
    """显示客户端 IP（用于验证 X-Real-IP / X-Forwarded-For）"""
    lines = []
    if request.headers.get("x-real-ip"):
        lines.append(f"X-Real-IP: {request.headers['x-real-ip']}")
    if request.headers.get("x-forwarded-for"):
        lines.append(f"X-Forwarded-For: {request.headers['x-forwarded-for']}")
    lines.append(f"Remote: {request.client.host}:{request.client.port}")
    return PlainTextResponse("\n".join(lines))


@app.get("/status/{code}")
async def status_code(code: int):
    """返回指定 HTTP 状态码"""
    messages = {
        200: "OK", 301: "Moved Permanently",
        302: "Found", 400: "Bad Request",
        403: "Forbidden", 404: "Not Found",
        500: "Internal Server Error", 502: "Bad Gateway",
        503: "Service Unavailable",
    }
    return PlainTextResponse(
        messages.get(code, f"Status {code}"),
        status_code=code,
    )


@app.get("/large")
async def large_body(size: int = 8192):
    """返回指定大小的响应体"""
    return PlainTextResponse("X" * size)


@app.get("/slow")
async def slow(delay: float = 2.0):
    """延迟响应（测试超时）"""
    await asyncio.sleep(delay)
    return PlainTextResponse(f"Delayed {delay}s")


@app.get("/chunked")
async def chunked():
    """分块传输响应"""
    async def generate():
        for i in range(10):
            yield f"chunk {i}\n".encode()
            await asyncio.sleep(0.1)
    return StreamingResponse(generate(), media_type="text/plain")


@app.websocket("/ws")
async def websocket_echo(ws: WebSocket):
    """WebSocket echo + 广播"""
    await ws.accept()
    try:
        while True:
            data = await ws.receive()
            if data["type"] == "websocket.receive":
                text = data.get("text")
                binary = data.get("bytes")
                if text is not None:
                    await ws.send_text(f"echo: {text}")
                elif binary is not None:
                    await ws.send_bytes(binary)
            elif data["type"] == "websocket.disconnect":
                break
    except WebSocketDisconnect:
        pass



@app.get("/hidden/secret", include_in_schema=False)
async def hidden_secret():
    """直接访问可见，代理不可见（用于测试路由隐藏）"""
    return PlainTextResponse("You found the secret!")


@app.get("/hidden/admin", include_in_schema=False)
async def hidden_admin(request: Request):
    """内部管理接口，不应被代理暴露"""
    return PlainTextResponse(
        f"Admin panel\n"
        f"Host: {request.headers.get('host', 'unknown')}\n"
        f"X-Forwarded-For: {request.headers.get('x-forwarded-for', 'none')}"
    )

if __name__ == "__main__":
    print(f"FastAPI backend on :{PORT}")
    uvicorn.run(app, host="0.0.0.0", port=PORT,
                log_level="info", access_log=True)
