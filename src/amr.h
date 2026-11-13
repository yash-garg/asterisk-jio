/* AMR format attributes shared by the SDP module and codec.
 * Based on traud/asterisk-amr (Unlicense), ported to Asterisk 22.
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef AST_AMR_H
#define AST_AMR_H

struct amr_attr {
	unsigned int octet_align;
	unsigned int mode_set; /* zero means all modes */
	unsigned int mode_change_period;
	unsigned int mode_change_neighbor;
	int max_red; /* -1 means unspecified */
	unsigned int specified; /* distinguishes wire attributes from local capabilities */
};

int amr_register_format_interfaces(void);
void amr_formats_ready(int ready);

#endif
