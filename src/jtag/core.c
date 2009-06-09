/***************************************************************************
 *   Copyright (C) 2005 by Dominic Rath                                    *
 *   Dominic.Rath@gmx.de                                                   *
 *                                                                         *
 *   Copyright (C) 2007,2008 �yvind Harboe                                 *
 *   oyvind.harboe@zylin.com                                               *
 *                                                                         *
 *   Copyright (C) 2009 SoftPLC Corporation                                *
 *       http://softplc.com                                                *
 *   dick@softplc.com                                                      *
 *                                                                         *
 *   Copyright (C) 2009 Zachary T Welch                                    *
 *   zw@superlucidity.net                                                  *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************/
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "jtag.h"
#include "minidriver.h"
#include "interface.h"

#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif


/// The number of JTAG queue flushes (for profiling and debugging purposes).
static int jtag_flush_queue_count;

static void jtag_add_scan_check(void (*jtag_add_scan)(int in_num_fields, const scan_field_t *in_fields, tap_state_t state),
		int in_num_fields, scan_field_t *in_fields, tap_state_t state);

/**
 * The jtag_error variable is set when an error occurs while executing
 * the queue.  Application code may set this using jtag_set_error(),
 * when an error occurs during processing that should be reported during
 * jtag_execute_queue().
 *
 * Tts value may be checked with jtag_get_error() and cleared with
 * jtag_error_clear().  This value is returned (and cleared) by
 * jtag_execute_queue().
 */
static int jtag_error = ERROR_OK;

static char* jtag_event_strings[] =
{
	"JTAG controller reset (RESET or TRST)"
};

const Jim_Nvp nvp_jtag_tap_event[] = {
	{ .value = JTAG_TAP_EVENT_ENABLE,       .name = "tap-enable" },
	{ .value = JTAG_TAP_EVENT_DISABLE,      .name = "tap-disable" },

	{ .name = NULL, .value = -1 }
};

static int jtag_trst = 0;
static int jtag_srst = 0;

/**
 * List all TAPs that have been created.
 */
static jtag_tap_t *__jtag_all_taps = NULL;
/**
 * The number of TAPs in the __jtag_all_taps list, used to track the
 * assigned chain position to new TAPs
 */
static unsigned jtag_num_taps = 0;

static enum reset_types jtag_reset_config = RESET_NONE;
static tap_state_t cmd_queue_end_state = TAP_RESET;
tap_state_t cmd_queue_cur_state = TAP_RESET;

int jtag_verify_capture_ir = 1;
static int jtag_verify = 1;

/* how long the OpenOCD should wait before attempting JTAG communication after reset lines deasserted (in ms) */
static int jtag_nsrst_delay = 0; /* default to no nSRST delay */
static int jtag_ntrst_delay = 0; /* default to no nTRST delay */

/* callbacks to inform high-level handlers about JTAG state changes */
static jtag_event_callback_t *jtag_event_callbacks;

/* speed in kHz*/
static int speed_khz = 0;
/* flag if the kHz speed was defined */
bool hasKHz = false;

struct jtag_interface_s *jtag = NULL;

/* configuration */
jtag_interface_t *jtag_interface = NULL;
static int jtag_speed = 0;

void jtag_set_error(int error)
{
	if ((error == ERROR_OK) || (jtag_error != ERROR_OK))
		return;
	jtag_error = error;
}
int jtag_get_error(void)
{
	return jtag_error;
}
int jtag_error_clear(void)
{
	int temp = jtag_error;
	jtag_error = ERROR_OK;
	return temp;
}


jtag_tap_t *jtag_all_taps(void)
{
	return __jtag_all_taps;
};

unsigned jtag_tap_count(void)
{
	return jtag_num_taps;
}

unsigned jtag_tap_count_enabled(void)
{
	jtag_tap_t *t = jtag_all_taps();
	unsigned n = 0;
	while(t)
	{
		if (t->enabled)
			n++;
		t = t->next_tap;
	}
	return n;
}

/// Append a new TAP to the chain of all taps.
void jtag_tap_add(struct jtag_tap_s *t)
{
	t->abs_chain_position = jtag_num_taps++;

	jtag_tap_t **tap = &__jtag_all_taps;
	while(*tap != NULL)
		tap = &(*tap)->next_tap;
	*tap = t;
}

jtag_tap_t *jtag_tap_by_string(const char *s)
{
	/* try by name first */
	jtag_tap_t *t = jtag_all_taps();
	while (t)
	{
		if (0 == strcmp(t->dotted_name, s))
			return t;
		t = t->next_tap;
	}

	/* no tap found by name, so try to parse the name as a number */
	char *cp;
	unsigned n = strtoul(s, &cp, 0);
	if ((s == cp) || (*cp != 0))
		return NULL;

	return jtag_tap_by_position(n);
}

jtag_tap_t *jtag_tap_by_jim_obj(Jim_Interp *interp, Jim_Obj *o)
{
	const char *cp = Jim_GetString(o, NULL);
	jtag_tap_t *t = cp ? jtag_tap_by_string(cp) : NULL;
	if (NULL == cp)
		cp = "(unknown)";
	if (NULL == t)
		Jim_SetResult_sprintf(interp, "Tap '%s' could not be found", cp);
	return t;
}

/* returns a pointer to the n-th device in the scan chain */
jtag_tap_t *jtag_tap_by_position(unsigned n)
{
	jtag_tap_t *t = jtag_all_taps();

	while (t && n-- > 0)
		t = t->next_tap;

	return t;
}

const char *jtag_tap_name(const jtag_tap_t *tap)
{
	return (tap == NULL) ? "(unknown)" : tap->dotted_name;
}


int jtag_register_event_callback(int (*callback)(enum jtag_event event, void *priv), void *priv)
{
	jtag_event_callback_t **callbacks_p = &jtag_event_callbacks;

	if (callback == NULL)
	{
		return ERROR_INVALID_ARGUMENTS;
	}

	if (*callbacks_p)
	{
		while ((*callbacks_p)->next)
			callbacks_p = &((*callbacks_p)->next);
		callbacks_p = &((*callbacks_p)->next);
	}

	(*callbacks_p) = malloc(sizeof(jtag_event_callback_t));
	(*callbacks_p)->callback = callback;
	(*callbacks_p)->priv = priv;
	(*callbacks_p)->next = NULL;

	return ERROR_OK;
}

int jtag_unregister_event_callback(int (*callback)(enum jtag_event event, void *priv))
{
	jtag_event_callback_t **callbacks_p = &jtag_event_callbacks;

	if (callback == NULL)
	{
		return ERROR_INVALID_ARGUMENTS;
	}

	while (*callbacks_p)
	{
		jtag_event_callback_t **next = &((*callbacks_p)->next);
		if ((*callbacks_p)->callback == callback)
		{
			free(*callbacks_p);
			*callbacks_p = *next;
		}
		callbacks_p = next;
	}

	return ERROR_OK;
}

int jtag_call_event_callbacks(enum jtag_event event)
{
	jtag_event_callback_t *callback = jtag_event_callbacks;

	LOG_DEBUG("jtag event: %s", jtag_event_strings[event]);

	while (callback)
	{
		callback->callback(event, callback->priv);
		callback = callback->next;
	}

	return ERROR_OK;
}

static void jtag_checks(void)
{
	assert(jtag_trst == 0);
}

static void jtag_prelude(tap_state_t state)
{
	jtag_checks();

	assert(state!=TAP_INVALID);

	cmd_queue_cur_state = state;
}

void jtag_alloc_in_value32(scan_field_t *field)
{
	interface_jtag_alloc_in_value32(field);
}

void jtag_add_ir_scan_noverify(int in_count, const scan_field_t *in_fields,
		tap_state_t state)
{
	jtag_prelude(state);

	int retval = interface_jtag_add_ir_scan(in_count, in_fields, state);
	jtag_set_error(retval);
}


void jtag_add_ir_scan(int in_num_fields, scan_field_t *in_fields, tap_state_t state)
{
	if (jtag_verify&&jtag_verify_capture_ir)
	{
		/* 8 x 32 bit id's is enough for all invocations */

		for (int j = 0; j < in_num_fields; j++)
		{
			/* if we are to run a verification of the ir scan, we need to get the input back.
			 * We may have to allocate space if the caller didn't ask for the input back.
			 */
			in_fields[j].check_value=in_fields[j].tap->expected;
			in_fields[j].check_mask=in_fields[j].tap->expected_mask;
		}
		jtag_add_scan_check(jtag_add_ir_scan_noverify, in_num_fields, in_fields, state);
	} else
	{
		jtag_add_ir_scan_noverify(in_num_fields, in_fields, state);
	}
}

void jtag_add_plain_ir_scan(int in_num_fields, const scan_field_t *in_fields,
		tap_state_t state)
{
	jtag_prelude(state);

	int retval = interface_jtag_add_plain_ir_scan(
			in_num_fields, in_fields, state);
	jtag_set_error(retval);
}

void jtag_add_callback(jtag_callback1_t f, u8 *in)
{
	interface_jtag_add_callback(f, in);
}

void jtag_add_callback4(jtag_callback_t f, u8 *in,
		jtag_callback_data_t data1, jtag_callback_data_t data2,
		jtag_callback_data_t data3)
{
	interface_jtag_add_callback4(f, in, data1, data2, data3);
}

int jtag_check_value_inner(u8 *captured, u8 *in_check_value, u8 *in_check_mask, int num_bits);

static int jtag_check_value_mask_callback(u8 *in, jtag_callback_data_t data1, jtag_callback_data_t data2, jtag_callback_data_t data3)
{
	return jtag_check_value_inner(in, (u8 *)data1, (u8 *)data2, (int)data3);
}

static void jtag_add_scan_check(void (*jtag_add_scan)(int in_num_fields, const scan_field_t *in_fields, tap_state_t state),
		int in_num_fields, scan_field_t *in_fields, tap_state_t state)
{
	for (int i = 0; i < in_num_fields; i++)
	{
		struct scan_field_s *field = &in_fields[i];
		field->allocated = 0;
		field->modified = 0;
		if (field->check_value || field->in_value)
			continue;
		interface_jtag_add_scan_check_alloc(field);
		field->modified = 1;
	}

	jtag_add_scan(in_num_fields, in_fields, state);

	for (int i = 0; i < in_num_fields; i++)
	{
		if ((in_fields[i].check_value != NULL) && (in_fields[i].in_value != NULL))
		{
			/* this is synchronous for a minidriver */
			jtag_add_callback4(jtag_check_value_mask_callback, in_fields[i].in_value,
				(jtag_callback_data_t)in_fields[i].check_value,
				(jtag_callback_data_t)in_fields[i].check_mask,
				(jtag_callback_data_t)in_fields[i].num_bits);
		}
		if (in_fields[i].allocated)
		{
			free(in_fields[i].in_value);
		}
		if (in_fields[i].modified)
		{
			in_fields[i].in_value = NULL;
		}
	}
}

void jtag_add_dr_scan_check(int in_num_fields, scan_field_t *in_fields, tap_state_t state)
{
	if (jtag_verify)
	{
		jtag_add_scan_check(jtag_add_dr_scan, in_num_fields, in_fields, state);
	} else
	{
		jtag_add_dr_scan(in_num_fields, in_fields, state);
	}
}


void jtag_add_dr_scan(int in_num_fields, const scan_field_t *in_fields,
		tap_state_t state)
{
	jtag_prelude(state);

	int retval;
	retval = interface_jtag_add_dr_scan(in_num_fields, in_fields, state);
	jtag_set_error(retval);
}

void jtag_add_plain_dr_scan(int in_num_fields, const scan_field_t *in_fields,
		tap_state_t state)
{
	jtag_prelude(state);

	int retval;
	retval = interface_jtag_add_plain_dr_scan(in_num_fields, in_fields, state);
	jtag_set_error(retval);
}

void jtag_add_dr_out(jtag_tap_t* tap,
		int num_fields, const int* num_bits, const u32* value,
		tap_state_t end_state)
{
	assert(end_state != TAP_INVALID);

	cmd_queue_cur_state = end_state;

	interface_jtag_add_dr_out(tap,
			num_fields, num_bits, value,
			end_state);
}

void jtag_add_tlr(void)
{
	jtag_prelude(TAP_RESET);
	jtag_set_error(interface_jtag_add_tlr());
	jtag_call_event_callbacks(JTAG_TRST_ASSERTED);
}

void jtag_add_pathmove(int num_states, const tap_state_t *path)
{
	tap_state_t cur_state = cmd_queue_cur_state;

	/* the last state has to be a stable state */
	if (!tap_is_state_stable(path[num_states - 1]))
	{
		LOG_ERROR("BUG: TAP path doesn't finish in a stable state");
		jtag_set_error(ERROR_JTAG_NOT_STABLE_STATE);
		return;
	}

	for (int i = 0; i < num_states; i++)
	{
		if (path[i] == TAP_RESET)
		{
			LOG_ERROR("BUG: TAP_RESET is not a valid state for pathmove sequences");
			jtag_set_error(ERROR_JTAG_STATE_INVALID);
			return;
		}

		if ( tap_state_transition(cur_state, true)  != path[i]
		  && tap_state_transition(cur_state, false) != path[i])
		{
			LOG_ERROR("BUG: %s -> %s isn't a valid TAP transition",
					tap_state_name(cur_state), tap_state_name(path[i]));
			jtag_set_error(ERROR_JTAG_TRANSITION_INVALID);
			return;
		}
		cur_state = path[i];
	}

	jtag_checks();

	jtag_set_error(interface_jtag_add_pathmove(num_states, path));
	cmd_queue_cur_state = path[num_states - 1];
}

void jtag_add_runtest(int num_cycles, tap_state_t state)
{
	jtag_prelude(state);
	jtag_set_error(interface_jtag_add_runtest(num_cycles, state));
}


void jtag_add_clocks(int num_cycles)
{
	if (!tap_is_state_stable(cmd_queue_cur_state))
	{
		 LOG_ERROR("jtag_add_clocks() called with TAP in unstable state \"%s\"",
				 tap_state_name(cmd_queue_cur_state));
		 jtag_set_error(ERROR_JTAG_NOT_STABLE_STATE);
		 return;
	}

	if (num_cycles > 0)
	{
		jtag_checks();
		jtag_set_error(interface_jtag_add_clocks(num_cycles));
	}
}

void jtag_add_reset(int req_tlr_or_trst, int req_srst)
{
	int trst_with_tlr = 0;

	/* FIX!!! there are *many* different cases here. A better
	 * approach is needed for legal combinations of transitions...
	 */
	if ((jtag_reset_config & RESET_HAS_SRST)&&
			(jtag_reset_config & RESET_HAS_TRST)&&
			((jtag_reset_config & RESET_SRST_PULLS_TRST)==0))
	{
		if (((req_tlr_or_trst&&!jtag_trst)||
				(!req_tlr_or_trst&&jtag_trst))&&
				((req_srst&&!jtag_srst)||
						(!req_srst&&jtag_srst)))
		{
			/* FIX!!! srst_pulls_trst allows 1,1 => 0,0 transition.... */
			//LOG_ERROR("BUG: transition of req_tlr_or_trst and req_srst in the same jtag_add_reset() call is undefined");
		}
	}

	/* Make sure that jtag_reset_config allows the requested reset */
	/* if SRST pulls TRST, we can't fulfill srst == 1 with trst == 0 */
	if (((jtag_reset_config & RESET_SRST_PULLS_TRST) && (req_srst == 1)) && (!req_tlr_or_trst))
	{
		LOG_ERROR("BUG: requested reset would assert trst");
		jtag_set_error(ERROR_FAIL);
		return;
	}

	/* if TRST pulls SRST, we reset with TAP T-L-R */
	if (((jtag_reset_config & RESET_TRST_PULLS_SRST) && (req_tlr_or_trst)) && (req_srst == 0))
	{
		trst_with_tlr = 1;
	}

	if (req_srst && !(jtag_reset_config & RESET_HAS_SRST))
	{
		LOG_ERROR("BUG: requested SRST assertion, but the current configuration doesn't support this");
		jtag_set_error(ERROR_FAIL);
		return;
	}

	if (req_tlr_or_trst)
	{
		if (!trst_with_tlr && (jtag_reset_config & RESET_HAS_TRST))
		{
			jtag_trst = 1;
		} else
		{
			trst_with_tlr = 1;
		}
	} else
	{
		jtag_trst = 0;
	}

	jtag_srst = req_srst;

	int retval = interface_jtag_add_reset(jtag_trst, jtag_srst);
	if (retval != ERROR_OK)
	{
		jtag_set_error(retval);
		return;
	}
	jtag_execute_queue();

	if (jtag_srst)
	{
		LOG_DEBUG("SRST line asserted");
	}
	else
	{
		LOG_DEBUG("SRST line released");
		if (jtag_nsrst_delay)
			jtag_add_sleep(jtag_nsrst_delay * 1000);
	}

	if (trst_with_tlr)
	{
		LOG_DEBUG("JTAG reset with RESET instead of TRST");
		jtag_set_end_state(TAP_RESET);
		jtag_add_tlr();
		return;
	}

	if (jtag_trst)
	{
		/* we just asserted nTRST, so we're now in Test-Logic-Reset,
		 * and inform possible listeners about this
		 */
		LOG_DEBUG("TRST line asserted");
		tap_set_state(TAP_RESET);
		jtag_call_event_callbacks(JTAG_TRST_ASSERTED);
	}
	else
	{
		if (jtag_ntrst_delay)
			jtag_add_sleep(jtag_ntrst_delay * 1000);
	}
}

tap_state_t jtag_set_end_state(tap_state_t state)
{
	if ((state == TAP_DRSHIFT)||(state == TAP_IRSHIFT))
	{
		LOG_ERROR("BUG: TAP_DRSHIFT/IRSHIFT can't be end state. Calling code should use a larger scan field");
	}

	if (state!=TAP_INVALID)
		cmd_queue_end_state = state;
	return cmd_queue_end_state;
}

tap_state_t jtag_get_end_state(void)
{
	return cmd_queue_end_state;
}

void jtag_add_sleep(u32 us)
{
	/// @todo Here, keep_alive() appears to be a layering violation!!!
	keep_alive();
	jtag_set_error(interface_jtag_add_sleep(us));
}

int jtag_check_value_inner(u8 *captured, u8 *in_check_value, u8 *in_check_mask, int num_bits)
{
	int retval = ERROR_OK;

	int compare_failed = 0;

	if (in_check_mask)
		compare_failed = buf_cmp_mask(captured, in_check_value, in_check_mask, num_bits);
	else
		compare_failed = buf_cmp(captured, in_check_value, num_bits);

	if (compare_failed){
		/* An error handler could have caught the failing check
		 * only report a problem when there wasn't a handler, or if the handler
		 * acknowledged the error
		 */
		/*
		LOG_WARNING("TAP %s:",
					jtag_tap_name(field->tap));
					*/
		if (compare_failed)
		{
			char *captured_char = buf_to_str(captured, (num_bits > DEBUG_JTAG_IOZ) ? DEBUG_JTAG_IOZ : num_bits, 16);
			char *in_check_value_char = buf_to_str(in_check_value, (num_bits > DEBUG_JTAG_IOZ) ? DEBUG_JTAG_IOZ : num_bits, 16);

			if (in_check_mask)
			{
				char *in_check_mask_char;
				in_check_mask_char = buf_to_str(in_check_mask, (num_bits > DEBUG_JTAG_IOZ) ? DEBUG_JTAG_IOZ : num_bits, 16);
				LOG_WARNING("value captured during scan didn't pass the requested check:");
				LOG_WARNING("captured: 0x%s check_value: 0x%s check_mask: 0x%s",
							captured_char, in_check_value_char, in_check_mask_char);
				free(in_check_mask_char);
			}
			else
			{
				LOG_WARNING("value captured during scan didn't pass the requested check: captured: 0x%s check_value: 0x%s", captured_char, in_check_value_char);
			}

			free(captured_char);
			free(in_check_value_char);

			retval = ERROR_JTAG_QUEUE_FAILED;
		}

	}
	return retval;
}

void jtag_check_value_mask(scan_field_t *field, u8 *value, u8 *mask)
{
	assert(field->in_value != NULL);

	if (value==NULL)
	{
		/* no checking to do */
		return;
	}

	jtag_execute_queue_noclear();

	int retval=jtag_check_value_inner(field->in_value, value, mask, field->num_bits);
	jtag_set_error(retval);
}



int default_interface_jtag_execute_queue(void)
{
	if (NULL == jtag)
	{
		LOG_ERROR("No JTAG interface configured yet.  "
			"Issue 'init' command in startup scripts "
			"before communicating with targets.");
		return ERROR_FAIL;
	}

	return jtag->execute_queue();
}

void jtag_execute_queue_noclear(void)
{
	jtag_flush_queue_count++;
	jtag_set_error(interface_jtag_execute_queue());
}

int jtag_get_flush_queue_count(void)
{
	return jtag_flush_queue_count;
}

int jtag_execute_queue(void)
{
	jtag_execute_queue_noclear();
	return jtag_error_clear();
}

static int jtag_reset_callback(enum jtag_event event, void *priv)
{
	jtag_tap_t *tap = priv;

	LOG_DEBUG("-");

	if (event == JTAG_TRST_ASSERTED)
	{
		buf_set_ones(tap->cur_instr, tap->ir_length);
		tap->bypass = 1;
	}

	return ERROR_OK;
}

void jtag_sleep(u32 us)
{
	alive_sleep(us/1000);
}

/// maximum number of JTAG devices expected in the chain
#define JTAG_MAX_CHAIN_SIZE 20

#define EXTRACT_MFG(X)  (((X) & 0xffe) >> 1)
#define EXTRACT_PART(X) (((X) & 0xffff000) >> 12)
#define EXTRACT_VER(X)  (((X) & 0xf0000000) >> 28)

static int jtag_examine_chain_execute(u8 *idcode_buffer, unsigned num_idcode)
{
	scan_field_t field = {
			.tap = NULL,
			.num_bits = num_idcode * 32,
			.out_value = idcode_buffer,
			.in_value = idcode_buffer,
		};

	// initialize to the end of chain ID value
	for (unsigned i = 0; i < JTAG_MAX_CHAIN_SIZE; i++)
		buf_set_u32(idcode_buffer, i * 32, 32, 0x000000FF);

	jtag_add_plain_dr_scan(1, &field, TAP_RESET);
	return jtag_execute_queue();
}

static bool jtag_examine_chain_check(u8 *idcodes, unsigned count)
{
	u8 zero_check = 0x0;
	u8 one_check = 0xff;

	for (unsigned i = 0; i < count * 4; i++)
	{
		zero_check |= idcodes[i];
		one_check &= idcodes[i];
	}

	/* if there wasn't a single non-zero bit or if all bits were one,
	 * the scan is not valid */
	if (zero_check == 0x00 || one_check == 0xff)
	{
		LOG_ERROR("JTAG communication failure: check connection, "
			"JTAG interface, target power etc.");
		return false;
	}
	return true;
}

static void jtag_examine_chain_display(enum log_levels level, const char *msg,
		const char *name, u32 idcode)
{
	log_printf_lf(level, __FILE__, __LINE__, __FUNCTION__,
			"JTAG tap: %s %16.16s: 0x%08x "
			"(mfg: 0x%3.3x, part: 0x%4.4x, ver: 0x%1.1x)",
		name, msg, idcode,
		EXTRACT_MFG(idcode), EXTRACT_PART(idcode), EXTRACT_VER(idcode) );
}

static bool jtag_idcode_is_final(u32 idcode)
{
		return idcode == 0x000000FF || idcode == 0xFFFFFFFF;
}

/**
 * This helper checks that remaining bits in the examined chain data are
 * all as expected, but a single JTAG device requires only 64 bits to be
 * read back correctly.  This can help identify and diagnose problems
 * with the JTAG chain earlier, gives more helpful/explicit error messages.
 */
static void jtag_examine_chain_end(u8 *idcodes, unsigned count, unsigned max)
{
	bool triggered = false;
	for ( ; count < max - 31; count += 32)
	{
		u32 idcode = buf_get_u32(idcodes, count, 32);
		// do not trigger the warning if the data looks good
		if (!triggered && jtag_idcode_is_final(idcode))
			continue;
		LOG_WARNING("Unexpected idcode after end of chain: %d 0x%08x",
				count, idcode);
		triggered = true;
	}
}

static bool jtag_examine_chain_match_tap(const struct jtag_tap_s *tap)
{
	if (0 == tap->expected_ids_cnt)
	{
		/// @todo Enable LOG_INFO to ask for reports about unknown TAP IDs.
#if 0
		LOG_INFO("Uknown JTAG TAP ID: 0x%08x", tap->idcode)
		LOG_INFO("Please report the chip name and reported ID code to the openocd project");
#endif
		return true;
	}

	/* Loop over the expected identification codes and test for a match */
	u8 ii;
	for (ii = 0; ii < tap->expected_ids_cnt; ii++)
	{
		if (tap->idcode == tap->expected_ids[ii])
			break;
	}

	/* If none of the expected ids matched, log an error */
	if (ii != tap->expected_ids_cnt)
	{
		LOG_INFO("JTAG Tap/device matched");
		return true;
	}
	jtag_examine_chain_display(LOG_LVL_ERROR, "got",
			tap->dotted_name, tap->idcode);
	for (ii = 0; ii < tap->expected_ids_cnt; ii++)
	{
		char msg[32];
		snprintf(msg, sizeof(msg), "expected %hhu of %hhu",
				ii + 1, tap->expected_ids_cnt);
		jtag_examine_chain_display(LOG_LVL_ERROR, msg,
				tap->dotted_name, tap->expected_ids[ii]);
	}
	return false;
}

/* Try to examine chain layout according to IEEE 1149.1 §12
 */
int jtag_examine_chain(void)
{
	u8 idcode_buffer[JTAG_MAX_CHAIN_SIZE * 4];
	unsigned device_count = 0;

	jtag_examine_chain_execute(idcode_buffer, JTAG_MAX_CHAIN_SIZE);

	if (!jtag_examine_chain_check(idcode_buffer, JTAG_MAX_CHAIN_SIZE))
		return ERROR_JTAG_INIT_FAILED;

	/* point at the 1st tap */
	jtag_tap_t *tap = jtag_tap_next_enabled(NULL);
	if (tap == NULL)
	{
		LOG_ERROR("JTAG: No taps enabled?");
		return ERROR_JTAG_INIT_FAILED;
	}

	for (unsigned bit_count = 0; bit_count < (JTAG_MAX_CHAIN_SIZE * 32) - 31;)
	{
		u32 idcode = buf_get_u32(idcode_buffer, bit_count, 32);
		if ((idcode & 1) == 0)
		{
			/* LSB must not be 0, this indicates a device in bypass */
			LOG_WARNING("Tap/Device does not have IDCODE");
			idcode = 0;

			bit_count += 1;
		}
		else
		{
	 		/*
			 * End of chain (invalid manufacturer ID) some devices, such
			 * as AVR will output all 1's instead of TDI input value at
			 * end of chain.
			 */
			if (jtag_idcode_is_final(idcode))
			{
				jtag_examine_chain_end(idcode_buffer,
						bit_count + 32, JTAG_MAX_CHAIN_SIZE * 32);
				break;
			}

			jtag_examine_chain_display(LOG_LVL_INFO, "tap/device found",
					tap ? tap->dotted_name : "(not-named)",
					idcode);

			bit_count += 32;
		}
		device_count++;
		if (!tap)
			continue;

		tap->idcode = idcode;

		// ensure the TAP ID does matches what was expected
 		if (!jtag_examine_chain_match_tap(tap))
			return ERROR_JTAG_INIT_FAILED;

		tap = jtag_tap_next_enabled(tap);
	}

	/* see if number of discovered devices matches configuration */
	if (device_count != jtag_tap_count_enabled())
	{
		LOG_ERROR("number of discovered devices in JTAG chain (%i) "
				"does not match (enabled) configuration (%i), total taps: %d",
				device_count, jtag_tap_count_enabled(), jtag_tap_count());
		LOG_ERROR("check the config file and ensure proper JTAG communication"
				" (connections, speed, ...)");
		return ERROR_JTAG_INIT_FAILED;
	}

	return ERROR_OK;
}

int jtag_validate_chain(void)
{
	jtag_tap_t *tap;
	int total_ir_length = 0;
	u8 *ir_test = NULL;
	scan_field_t field;
	int chain_pos = 0;

	tap = NULL;
	total_ir_length = 0;
	for(;;){
		tap = jtag_tap_next_enabled(tap);
		if( tap == NULL ){
			break;
		}
		total_ir_length += tap->ir_length;
	}

	total_ir_length += 2;
	ir_test = malloc(CEIL(total_ir_length, 8));
	buf_set_ones(ir_test, total_ir_length);

	field.tap = NULL;
	field.num_bits = total_ir_length;
	field.out_value = ir_test;
	field.in_value = ir_test;


	jtag_add_plain_ir_scan(1, &field, TAP_RESET);
	jtag_execute_queue();

	tap = NULL;
	chain_pos = 0;
	int val;
	for(;;){
		tap = jtag_tap_next_enabled(tap);
		if( tap == NULL ){
			break;
		}

		val = buf_get_u32(ir_test, chain_pos, 2);
		if (val != 0x1)
		{
			char *cbuf = buf_to_str(ir_test, total_ir_length, 16);
			LOG_ERROR("Could not validate JTAG scan chain, IR mismatch, scan returned 0x%s. tap=%s pos=%d expected 0x1 got %0x", cbuf, jtag_tap_name(tap), chain_pos, val);
			free(cbuf);
			free(ir_test);
			return ERROR_JTAG_INIT_FAILED;
		}
		chain_pos += tap->ir_length;
	}

	val = buf_get_u32(ir_test, chain_pos, 2);
	if (val != 0x3)
	{
		char *cbuf = buf_to_str(ir_test, total_ir_length, 16);
		LOG_ERROR("Could not validate end of JTAG scan chain, IR mismatch, scan returned 0x%s. pos=%d expected 0x3 got %0x", cbuf, chain_pos, val);
		free(cbuf);
		free(ir_test);
		return ERROR_JTAG_INIT_FAILED;
	}

	free(ir_test);

	return ERROR_OK;
}


void jtag_tap_init(jtag_tap_t *tap)
{
	assert(0 != tap->ir_length);

	tap->expected = malloc(tap->ir_length);
	tap->expected_mask = malloc(tap->ir_length);
	tap->cur_instr = malloc(tap->ir_length);

	buf_set_u32(tap->expected, 0, tap->ir_length, tap->ir_capture_value);
	buf_set_u32(tap->expected_mask, 0, tap->ir_length, tap->ir_capture_mask);
	buf_set_ones(tap->cur_instr, tap->ir_length);

	// place TAP in bypass mode
	tap->bypass = 1;
	// register the reset callback for the TAP
	jtag_register_event_callback(&jtag_reset_callback, tap);

	LOG_DEBUG("Created Tap: %s @ abs position %d, "
			"irlen %d, capture: 0x%x mask: 0x%x", tap->dotted_name,
				tap->abs_chain_position, tap->ir_length,
				tap->ir_capture_value, tap->ir_capture_mask);
	jtag_tap_add(tap);
}

void jtag_tap_free(jtag_tap_t *tap)
{
	/// @todo is anything missing? no memory leaks please 
	free((void *)tap->expected_ids);
	free((void *)tap->chip);
	free((void *)tap->tapname);
	free((void *)tap->dotted_name);
	free(tap);
}

int jtag_interface_init(struct command_context_s *cmd_ctx)
{
	if (jtag)
		return ERROR_OK;

	if (!jtag_interface)
	{
		/* nothing was previously specified by "interface" command */
		LOG_ERROR("JTAG interface has to be specified, see \"interface\" command");
		return ERROR_JTAG_INVALID_INTERFACE;
	}
	if(hasKHz)
	{
		jtag_interface->khz(jtag_get_speed_khz(), &jtag_speed);
		hasKHz = false;
	}

	if (jtag_interface->init() != ERROR_OK)
		return ERROR_JTAG_INIT_FAILED;

	jtag = jtag_interface;
	return ERROR_OK;
}

static int jtag_init_inner(struct command_context_s *cmd_ctx)
{
	jtag_tap_t *tap;
	int retval;

	LOG_DEBUG("Init JTAG chain");

	tap = jtag_tap_next_enabled(NULL);
	if( tap == NULL ){
		LOG_ERROR("There are no enabled taps?");
		return ERROR_JTAG_INIT_FAILED;
	}

	jtag_add_tlr();
	if ((retval=jtag_execute_queue())!=ERROR_OK)
		return retval;

	/* examine chain first, as this could discover the real chain layout */
	if (jtag_examine_chain() != ERROR_OK)
	{
		LOG_ERROR("trying to validate configured JTAG chain anyway...");
	}

	if (jtag_validate_chain() != ERROR_OK)
	{
		LOG_WARNING("Could not validate JTAG chain, continuing anyway...");
	}

	return ERROR_OK;
}

int jtag_interface_quit(void)
{
	if (!jtag || !jtag->quit)
		return ERROR_OK;

	// close the JTAG interface
	int result = jtag->quit();
	if (ERROR_OK != result)
		LOG_ERROR("failed: %d", result);

	return ERROR_OK;
}


int jtag_init_reset(struct command_context_s *cmd_ctx)
{
	int retval;

	if ((retval=jtag_interface_init(cmd_ctx)) != ERROR_OK)
		return retval;

	LOG_DEBUG("Trying to bring the JTAG controller to life by asserting TRST / RESET");

	/* Reset can happen after a power cycle.
	 *
	 * Ideally we would only assert TRST or run RESET before the target reset.
	 *
	 * However w/srst_pulls_trst, trst is asserted together with the target
	 * reset whether we want it or not.
	 *
	 * NB! Some targets have JTAG circuitry disabled until a
	 * trst & srst has been asserted.
	 *
	 * NB! here we assume nsrst/ntrst delay are sufficient!
	 *
	 * NB! order matters!!!! srst *can* disconnect JTAG circuitry
	 *
	 */
	jtag_add_reset(1, 0); /* RESET or TRST */
	if (jtag_reset_config & RESET_HAS_SRST)
	{
		jtag_add_reset(1, 1);
		if ((jtag_reset_config & RESET_SRST_PULLS_TRST)==0)
			jtag_add_reset(0, 1);
	}
	jtag_add_reset(0, 0);
	if ((retval = jtag_execute_queue()) != ERROR_OK)
		return retval;

	/* Check that we can communication on the JTAG chain + eventually we want to
	 * be able to perform enumeration only after OpenOCD has started
	 * telnet and GDB server
	 *
	 * That would allow users to more easily perform any magic they need to before
	 * reset happens.
	 */
	return jtag_init_inner(cmd_ctx);
}

int jtag_init(struct command_context_s *cmd_ctx)
{
	int retval;
	if ((retval=jtag_interface_init(cmd_ctx)) != ERROR_OK)
		return retval;
	if (jtag_init_inner(cmd_ctx)==ERROR_OK)
	{
		return ERROR_OK;
	}
	return jtag_init_reset(cmd_ctx);
}

void jtag_set_speed_khz(unsigned khz)
{
	speed_khz = khz;
}
unsigned jtag_get_speed_khz(void)
{
	return speed_khz;
}
int jtag_get_speed(void)
{
	return jtag_speed;
}

int jtag_set_speed(int speed)
{
	jtag_speed = speed;
	/* this command can be called during CONFIG,
	 * in which case jtag isn't initialized */
	return jtag ? jtag->speed(speed) : ERROR_OK;
}

void jtag_set_verify(bool enable)
{
	jtag_verify = enable;
}

bool jtag_will_verify()
{
	return jtag_verify;
}


int jtag_power_dropout(int *dropout)
{
	return jtag->power_dropout(dropout);
}

int jtag_srst_asserted(int *srst_asserted)
{
	return jtag->srst_asserted(srst_asserted);
}

void jtag_tap_handle_event( jtag_tap_t * tap, enum jtag_tap_event e)
{
	jtag_tap_event_action_t * jteap;
	int done;

	jteap = tap->event_action;

	done = 0;
	while (jteap) {
		if (jteap->event == e) {
			done = 1;
			LOG_DEBUG( "JTAG tap: %s event: %d (%s) action: %s\n",
					tap->dotted_name,
					e,
					Jim_Nvp_value2name_simple(nvp_jtag_tap_event, e)->name,
					Jim_GetString(jteap->body, NULL) );
			if (Jim_EvalObj(interp, jteap->body) != JIM_OK) {
				Jim_PrintErrorMessage(interp);
			}
		}

		jteap = jteap->next;
	}

	if (!done) {
		LOG_DEBUG( "event %d %s - no action",
				e,
				Jim_Nvp_value2name_simple( nvp_jtag_tap_event, e)->name);
	}
}

int jtag_add_statemove(tap_state_t goal_state)
{
	tap_state_t cur_state = cmd_queue_cur_state;

	LOG_DEBUG( "cur_state=%s goal_state=%s",
		tap_state_name(cur_state),
		tap_state_name(goal_state) );


	if (goal_state==cur_state )
		;	/* nothing to do */
	else if( goal_state==TAP_RESET )
	{
		jtag_add_tlr();
	}
	else if( tap_is_state_stable(cur_state) && tap_is_state_stable(goal_state) )
	{
		unsigned tms_bits  = tap_get_tms_path(cur_state, goal_state);
		unsigned tms_count = tap_get_tms_path_len(cur_state, goal_state);
		tap_state_t moves[8];
		assert(tms_count < DIM(moves));

		for (unsigned i = 0; i < tms_count; i++, tms_bits >>= 1)
		{
			bool bit = tms_bits & 1;

			cur_state = tap_state_transition(cur_state, bit);
			moves[i] = cur_state;
		}

		jtag_add_pathmove(tms_count, moves);
	}
	else if( tap_state_transition(cur_state, true)  == goal_state
		||   tap_state_transition(cur_state, false) == goal_state )
	{
		jtag_add_pathmove(1, &goal_state);
	}

	else
		return ERROR_FAIL;

	return ERROR_OK;
}

enum reset_types jtag_get_reset_config(void)
{
	return jtag_reset_config;
}
void jtag_set_reset_config(enum reset_types type)
{
	jtag_reset_config = type;
}

int jtag_get_trst(void)
{
	return jtag_trst;
}
int jtag_get_srst(void)
{
	return jtag_srst;
}

void jtag_set_nsrst_delay(unsigned delay)
{
	jtag_nsrst_delay = delay;
}
unsigned jtag_get_nsrst_delay(void)
{
	return jtag_nsrst_delay;
}
void jtag_set_ntrst_delay(unsigned delay)
{
	jtag_ntrst_delay = delay;
}
unsigned jtag_get_ntrst_delay(void)
{
	return jtag_ntrst_delay;
}
