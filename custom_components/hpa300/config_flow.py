"""Config flow for HPA300."""

from __future__ import annotations

from typing import Any
from urllib.parse import urlsplit

import voluptuous as vol

from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.const import CONF_HOST
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.selector import (
    TextSelector,
    TextSelectorConfig,
    TextSelectorType,
)

from .api import HPA300Api, HPA300AuthError, HPA300Error
from .const import CONF_API_TOKEN, DOMAIN


def _normalize_host(value: str) -> str:
    value = value.strip()
    parsed = urlsplit(value if "://" in value else f"//{value}")
    if parsed.scheme not in ("", "http"):
        raise ValueError("only HTTP is supported")
    if not parsed.hostname or parsed.username or parsed.password:
        raise ValueError("invalid host")
    if parsed.path not in ("", "/") or parsed.query or parsed.fragment:
        raise ValueError("enter a host, not an API URL")
    try:
        port = parsed.port
    except ValueError as err:
        raise ValueError("invalid port") from err
    host = f"[{parsed.hostname}]" if ":" in parsed.hostname else parsed.hostname
    return f"{host}:{port}" if port is not None else host


def _schema(defaults: dict[str, Any] | None = None) -> vol.Schema:
    defaults = defaults or {}
    host_key = vol.Required(CONF_HOST)
    if CONF_HOST in defaults:
        host_key = vol.Required(CONF_HOST, default=defaults[CONF_HOST])
    return vol.Schema(
        {
            host_key: TextSelector(TextSelectorConfig(type=TextSelectorType.TEXT)),
            vol.Required(CONF_API_TOKEN): TextSelector(
                TextSelectorConfig(type=TextSelectorType.PASSWORD)
            ),
        }
    )


class HPA300ConfigFlow(ConfigFlow, domain=DOMAIN):
    """Set up or reconfigure an HPA300."""

    VERSION = 1

    async def _validate(
        self, user_input: dict[str, Any]
    ) -> tuple[dict[str, Any], dict[str, Any]]:
        data = {
            CONF_HOST: _normalize_host(user_input[CONF_HOST]),
            CONF_API_TOKEN: user_input[CONF_API_TOKEN],
        }
        api = HPA300Api(
            async_get_clientsession(self.hass), data[CONF_HOST], data[CONF_API_TOKEN]
        )
        device = await api.async_get_device()
        await api.async_get_state()
        if not isinstance(device.get("id"), str) or not device["id"]:
            raise HPA300Error("device response does not contain an ID")
        return data, device

    async def async_step_user(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            try:
                data, device = await self._validate(user_input)
            except ValueError:
                errors["base"] = "invalid_host"
            except HPA300AuthError:
                errors["base"] = "invalid_auth"
            except HPA300Error:
                errors["base"] = "cannot_connect"
            else:
                await self.async_set_unique_id(device["id"])
                self._abort_if_unique_id_configured(updates={CONF_HOST: data[CONF_HOST]})
                return self.async_create_entry(
                    title=device.get("name", "HPA300"), data=data
                )
        return self.async_show_form(step_id="user", data_schema=_schema(), errors=errors)

    async def async_step_reauth(
        self, entry_data: dict[str, Any]
    ) -> ConfigFlowResult:
        return await self.async_step_reauth_confirm()

    async def async_step_reauth_confirm(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        entry = self._get_reauth_entry()
        return await self._async_update_entry("reauth_confirm", entry, user_input)

    async def async_step_reconfigure(
        self, user_input: dict[str, Any] | None = None
    ) -> ConfigFlowResult:
        entry = self._get_reconfigure_entry()
        return await self._async_update_entry("reconfigure", entry, user_input)

    async def _async_update_entry(
        self, step_id: str, entry, user_input: dict[str, Any] | None
    ) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            try:
                data, device = await self._validate(user_input)
            except ValueError:
                errors["base"] = "invalid_host"
            except HPA300AuthError:
                errors["base"] = "invalid_auth"
            except HPA300Error:
                errors["base"] = "cannot_connect"
            else:
                await self.async_set_unique_id(device["id"])
                self._abort_if_unique_id_mismatch()
                return self.async_update_reload_and_abort(entry, data_updates=data)
        return self.async_show_form(
            step_id=step_id,
            data_schema=_schema(entry.data),
            errors=errors,
        )
