"""Async client for the HPA300 local HTTP API."""

from __future__ import annotations

import asyncio
from typing import Any

from aiohttp import ClientError, ClientResponse, ClientSession, ClientTimeout


class HPA300Error(Exception):
    """Base HPA300 API error."""


class HPA300ConnectionError(HPA300Error):
    """The HPA300 could not be reached or returned an invalid response."""


class HPA300AuthError(HPA300Error):
    """The HPA300 rejected the API token."""


class HPA300CommandError(HPA300Error):
    """The HPA300 rejected a command."""


class HPA300Api:
    """Client for one HPA300 controller."""

    def __init__(self, session: ClientSession, host: str, token: str) -> None:
        self._session = session
        self.host = host
        self._token = token
        self._base_url = f"http://{host}"

    async def _json_request(
        self, method: str, path: str, *, json: dict[str, Any] | None = None
    ) -> dict[str, Any]:
        try:
            response = await self._session.request(
                method,
                f"{self._base_url}{path}",
                headers={"Authorization": f"Bearer {self._token}"},
                json=json,
                timeout=ClientTimeout(total=4),
            )
            async with response:
                return await self._decode_response(response)
        except HPA300Error:
            raise
        except (asyncio.TimeoutError, ClientError, ValueError) as err:
            raise HPA300ConnectionError(str(err)) from err

    @staticmethod
    async def _decode_response(response: ClientResponse) -> dict[str, Any]:
        if response.status == 401:
            raise HPA300AuthError("invalid API token")
        if response.status >= 400:
            try:
                body = await response.json(content_type=None)
                message = body.get("error", f"HTTP {response.status}")
            except (ClientError, ValueError):
                message = f"HTTP {response.status}"
            raise HPA300CommandError(message)
        data = await response.json(content_type=None)
        if not isinstance(data, dict):
            raise HPA300ConnectionError("the device returned invalid JSON")
        return data

    async def async_get_device(self) -> dict[str, Any]:
        """Return identity and diagnostics."""
        return await self._json_request("GET", "/api/v1/device")

    async def async_get_state(self) -> dict[str, Any]:
        """Return current fan state."""
        return await self._json_request("GET", "/api/v1/state")

    async def async_set_speed(self, speed: int) -> None:
        """Set a fan speed from zero through four."""
        if speed not in range(5):
            raise ValueError("speed must be from zero through four")
        await self._json_request("PUT", "/api/v1/fan", json={"speed": speed})

