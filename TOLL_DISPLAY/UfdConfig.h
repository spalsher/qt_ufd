#pragma once
// Set a unique secret before deployment. Empty allows isolated legacy testing only.
#define UFD_API_TOKEN ""
// Compatibility mode for existing booth clients. Disable after migration.
#define UFD_ALLOW_LEGACY 0
// Configuration/reboot/panel-test endpoints are disabled by default.
// To enable, set a NONEMPTY API token too; send Authorization: Bearer <token>.
#define UFD_ENABLE_ADMIN 0
#define UFD_ACTIVE_LEASE_MS 120000UL
