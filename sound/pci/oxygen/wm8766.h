#ifndef WM8766_H_INCLUDED
#define WM8766_H_INCLUDED

#define WM8766_LDA1		0x00
#define WM8766_RDA1		0x01
#define WM8766_DAC_CTRL		0x02
#define WM8766_INT_CTRL		0x03
#define WM8766_LDA2		0x04
#define WM8766_RDA2		0x05
#define WM8766_LDA3		0x06
#define WM8766_RDA3		0x07
#define WM8766_MASTDA		0x08
#define WM8766_DAC_CTRL2	0x09
#define WM8766_DAC_CTRL3	0x0a
#define WM8766_MUTE1		0x0c
#define WM8766_MUTE2		0x0f
#define WM8766_RESET		0x1f

/* LDAx/RDAx/MASTDA */
#define WM8766_ATT_MASK		0x0ff
#define WM8766_UPDATE		0x100
/* DAC_CTRL */
#define WM8766_MUTEALL		0x001
#define WM8766_DEEMPALL		0x002
#define WM8766_PWDN		0x004
#define WM8766_ATC		0x008
#define WM8766_IZD		0x010
#define WM8766_PL_LEFT_MASK	0x060
#define WM8766_PL_LEFT_MUTE	0x000
#define WM8766_PL_LEFT_LEFT	0x020
#define WM8766_PL_LEFT_RIGHT	0x040
#define WM8766_PL_LEFT_LRMIX	0x060
#define WM8766_PL_RIGHT_MASK	0x180
#define WM8766_PL_RIGHT_MUTE	0x000
#define WM8766_PL_RIGHT_LEFT	0x080
#define WM8766_PL_RIGHT_RIGHT	0x100
#define WM8766_PL_RIGHT_LRMIX	0x180
/* INT_CTRL */
#define WM8766_FMT_MASK		0x003
#define WM8766_FMT_RJUST	0x000
#define WM8766_FMT_LJUST	0x001
#define WM8766_FMT_I2S		0x002
#define WM8766_FMT_DSP		0x003
#define WM8766_LRP		0x004
#define WM8766_BCP		0x008
#define WM8766_IWL_MASK		0x030
#define WM8766_IWL_16		0x000
#define WM8766_IWL_20		0x010
#define WM8766_IWL_24		0x020
#define WM8766_IWL_32		0x030
#define WM8766_PHASE_MASK	0x1c0
/* DAC_CTRL2 */
#define WM8766_ZCD		0x001
#define WM8766_DZFM_MASK	0x006
#define WM8766_DMUTE_MASK	0x038
#define WM8766_DEEMP_MASK	0x1c0
/* DAC_CTRL3 */
#define WM8766_DACPD_MASK	0x00e
#define WM8766_PWRDNALL		0x010
#define WM8766_MS		0x020
#define WM8766_RATE_MASK	0x1c0
#define WM8766_RATE_128		0x000
#define WM8766_RATE_192		0x040
#define WM8766_RATE_256		0x080
#define WM8766_RATE_384		0x0c0
#define WM8766_RATE_512		0x100
#define WM8766_RATE_768		0x140
/* MUTE1 */
#define WM8766_MPD1		0x040
/* MUTE2 */
#define WM8766_MPD2		0x020

#endif
