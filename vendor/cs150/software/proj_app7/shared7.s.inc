#### Shared C/ISR constants.
#WARN: MUST keep offsets SYNCHRONIZED with "sharedN.h" struct & constants.
#    :   Can consider using GNU C Preprocessor on "sharedN.h"

#NOTE: Don't surround const values with parentheses in GAS...

## Basic constants:
.equiv  K_CPU_HZ,           50000000
.equiv  K_TIMER_HZ,         1
.equiv  K_TIMER_CYC,        K_TIMER_HZ * K_CPU_HZ

## Shared memory constants:
.equiv  K_MAJOR_VERSION,    6
.equiv  K_MAGIC_VERSION,    0xFEDBEEF2
.equiv  K_SHBUF_SIZEB,      0x0100
.equiv  K_SHBUF_ROLLOVERB,  0x00FF

## Shared memory base and field offsets (for IMM(RS) addressing from base):
.equiv  SM_BASE,            0x50000000
.equiv  SMO_magic1,         0x0000
.equiv  SMO_majorvers,      0x0004
.equiv  SMO_stash0,         0x0008
.equiv  SMO_stash1,         0x000C
.equiv  SMO_stash2,         0x0010
.equiv  SMO_stash3,         0x0014
.equiv  SMO_state,            0x0018    # Unsigned 16-bit
.equiv  SMO_flags,            0x001A    # Unsigned 16-bit
.equiv  SMO_RTC_count,      0x001C
.equiv  SMO_seconds,        0x0020
.equiv  SMO_clock,          0x0024
.equiv  SMO_player_y,         0x0028    # Signed 16-bit (use "lh" & "sh")
.equiv  SMO_player_x,         0x002A    # Signed 16-bit
.equiv  SMO_max_bouncers,     0x002C    # Unsigned 16-bit
.equiv  SMO_num_bouncers,     0x002E    # Unsigned 16-bit
.equiv  SMO_buff_size,      0x0030
.equiv  SMO_buff_head,      0x0034
.equiv  SMO_buff_tail,      0x0038
.equiv  SMO_buff_data,      0x003C
.equiv  SMO_buff_last_byte, SMO_buff_data + K_SHBUF_ROLLOVERB
.equiv  SMO_magic2,         SMO_buff_data + K_SHBUF_SIZEB
.equiv  SMO_PAST_END,       SMO_magic2 + 4

## Shared memory fields as individual addresses:
.equiv  SMA_magic1,         SM_BASE + SMO_magic1
.equiv  SMA_majorvers,      SM_BASE + SMO_majorvers
.equiv  SMA_stash0,         SM_BASE + SMO_stash0
.equiv  SMA_stash1,         SM_BASE + SMO_stash1
.equiv  SMA_stash2,         SM_BASE + SMO_stash2
.equiv  SMA_stash3,         SM_BASE + SMO_stash3
.equiv  SMA_state,            SM_BASE + SMO_state
.equiv  SMA_flags,            SM_BASE + SMO_flags
.equiv  SMA_RTC_count,      SM_BASE + SMO_RTC_count
.equiv  SMA_seconds,        SM_BASE + SMO_seconds
.equiv  SMA_clock,          SM_BASE + SMO_clock
.equiv  SMA_player_y,         SM_BASE + SMO_player_y
.equiv  SMA_player_x,         SM_BASE + SMO_player_x
.equiv  SMA_max_bouncers,     SM_BASE + SMO_max_bouncers
.equiv  SMA_num_bouncers,     SM_BASE + SMO_num_bouncers
.equiv  SMA_buff_size,      SM_BASE + SMO_buff_size
.equiv  SMA_buff_head,      SM_BASE + SMO_buff_head
.equiv  SMA_buff_tail,      SM_BASE + SMO_buff_tail
.equiv  SMA_buff_data,      SM_BASE + SMO_buff_data
.equiv  SMA_buff_end_byte,  SM_BASE + SMO_buff_last_byte
.equiv  SMA_magic2,         SM_BASE + SMO_magic2
.equiv  SMA_PAST_END,       SM_BASE + SMO_PAST_END

## Offsets and Masks (and inverted masks) for SM fields:
# Bit numbers (offsets?) for {SMO/SMA}_flags:
.equiv  SMB_flags_pause,    0
.equiv  SMB_flags_notimer,  1
.equiv  SMB_flags_quit,     2
# Flag masks for testing or setting flags
.equiv  SMM_flags_pause,    (1 << SMB_flags_pause) & 0xFFFF
.equiv  SMM_flags_notimer,  (1 << SMB_flags_notimer) & 0xFFFF
.equiv  SMM_flags_quit,     (1 << SMB_flags_quit) & 0xFFFF
# Inverted flag masks, kept at 16-bits, for clearing flags
.equiv  SMI_flags_pause,    ~SMM_flags_pause & 0xFFFF
.equiv  SMI_flags_notimer,  ~SMM_flags_notimer & 0xFFFF
.equiv  SMI_flags_quit,     ~SMM_flags_quit & 0xFFFF
#NOTE: Above masks (0xFFFF) help ensure unsigned 16-bit result
