/* SPDX-License-Identifier: MIT */
#pragma once

// HookSpec is declared by js_generated/hook_specs.h. No event allows native
// policy bypass except explicitly supported decisions gated by admin authority.
inline constexpr HookSpec EventHookSpecs[] = {
	{"accountChange", false, false, true},
	{"userPreAway", true, true, true},
	{"userPreBack", true, true, true},
	{"userAway", false, false, true},
	{"userBack", false, false, true},
	{"capAddDel", false, false, true},
	{"capValueChange", false, false, true},
	{"namesListItem", true, false, true},
	{"stats", true, false, true},
	{"whois", false, false, true},
	{"whoisLine", true, false, true},
	{"whoLine", true, true, true},
	{"whoMatch", true, true, true},
	{"whoVisible", true, true, true},
	{"serverLink", false, false, true},
	{"serverBurst", false, false, true},
	{"serverSplit", false, false, true},
	{"syncUser", false, false, true},
	{"syncChannel", false, false, true},
	{"syncNetwork", false, false, true},
	{"httpACLCheck", false, false, false},
	{"httpRequest", false, false, false},
};
