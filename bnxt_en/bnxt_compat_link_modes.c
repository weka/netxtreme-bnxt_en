/* THIS A GENERATED FILE - DO NOT EDIT!
 *
 * To regenerate, use: update_compat.sh
 */

#define __LINK_MODE_LANES_CR		1
#define __LINK_MODE_LANES_CR2		2
#define __LINK_MODE_LANES_CR4		4
#define __LINK_MODE_LANES_CR8		8
#define __LINK_MODE_LANES_DR		1
#define __LINK_MODE_LANES_DR2		2
#define __LINK_MODE_LANES_DR4		4
#define __LINK_MODE_LANES_DR8		8
#define __LINK_MODE_LANES_KR		1
#define __LINK_MODE_LANES_KR2		2
#define __LINK_MODE_LANES_KR4		4
#define __LINK_MODE_LANES_KR8		8
#define __LINK_MODE_LANES_SR		1
#define __LINK_MODE_LANES_SR2		2
#define __LINK_MODE_LANES_SR4		4
#define __LINK_MODE_LANES_SR8		8
#define __LINK_MODE_LANES_ER		1
#define __LINK_MODE_LANES_KX		1
#define __LINK_MODE_LANES_KX4		4
#define __LINK_MODE_LANES_LR		1
#define __LINK_MODE_LANES_LR4		4
#define __LINK_MODE_LANES_LR4_ER4	4
#define __LINK_MODE_LANES_LR_ER_FR	1
#define __LINK_MODE_LANES_LR2_ER2_FR2	2
#define __LINK_MODE_LANES_LR4_ER4_FR4	4
#define __LINK_MODE_LANES_LR8_ER8_FR8	8
#define __LINK_MODE_LANES_LRM		1
#define __LINK_MODE_LANES_MLD2		2
#define __LINK_MODE_LANES_T		1
#define __LINK_MODE_LANES_T1		1
#define __LINK_MODE_LANES_X		1
#define __LINK_MODE_LANES_FX		1
#define __DUPLEX_Half DUPLEX_HALF
#define __DUPLEX_Full DUPLEX_FULL
#define __DEFINE_LINK_MODE_PARAMS(_speed, _type, _duplex)	\
	[ETHTOOL_LINK_MODE(_speed, _type, _duplex)] = {		\
		.speed  = SPEED_ ## _speed, \
		.lanes  = __LINK_MODE_LANES_ ## _type, \
		.duplex	= __DUPLEX_ ## _duplex \
	}
#define __DEFINE_SPECIAL_MODE_PARAMS(_mode) \
	[ETHTOOL_LINK_MODE_ ## _mode ## _BIT] = { \
		.speed	= SPEED_UNKNOWN, \
		.lanes	= 0, \
		.duplex	= DUPLEX_UNKNOWN, \
	}
const struct link_mode_info link_mode_params[] = {
	__DEFINE_LINK_MODE_PARAMS(10, T, Half),
	__DEFINE_LINK_MODE_PARAMS(10, T, Full),
	__DEFINE_LINK_MODE_PARAMS(100, T, Half),
	__DEFINE_LINK_MODE_PARAMS(100, T, Full),
	__DEFINE_LINK_MODE_PARAMS(1000, T, Half),
	__DEFINE_LINK_MODE_PARAMS(1000, T, Full),
	__DEFINE_SPECIAL_MODE_PARAMS(Autoneg),
	__DEFINE_SPECIAL_MODE_PARAMS(TP),
	__DEFINE_SPECIAL_MODE_PARAMS(AUI),
	__DEFINE_SPECIAL_MODE_PARAMS(MII),
	__DEFINE_SPECIAL_MODE_PARAMS(FIBRE),
	__DEFINE_SPECIAL_MODE_PARAMS(BNC),
	__DEFINE_LINK_MODE_PARAMS(10000, T, Full),
	__DEFINE_SPECIAL_MODE_PARAMS(Pause),
	__DEFINE_SPECIAL_MODE_PARAMS(Asym_Pause),
	__DEFINE_LINK_MODE_PARAMS(2500, X, Full),
	__DEFINE_SPECIAL_MODE_PARAMS(Backplane),
	__DEFINE_LINK_MODE_PARAMS(1000, KX, Full),
	__DEFINE_LINK_MODE_PARAMS(10000, KX4, Full),
	__DEFINE_LINK_MODE_PARAMS(10000, KR, Full),
	[ETHTOOL_LINK_MODE_10000baseR_FEC_BIT] = {
		.speed	= SPEED_10000,
		.lanes	= 1,
		.duplex = DUPLEX_FULL,
	},
	__DEFINE_LINK_MODE_PARAMS(20000, MLD2, Full),
	__DEFINE_LINK_MODE_PARAMS(20000, KR2, Full),
	__DEFINE_LINK_MODE_PARAMS(40000, KR4, Full),
	__DEFINE_LINK_MODE_PARAMS(40000, CR4, Full),
	__DEFINE_LINK_MODE_PARAMS(40000, SR4, Full),
	__DEFINE_LINK_MODE_PARAMS(40000, LR4, Full),
	__DEFINE_LINK_MODE_PARAMS(56000, KR4, Full),
	__DEFINE_LINK_MODE_PARAMS(56000, CR4, Full),
	__DEFINE_LINK_MODE_PARAMS(56000, SR4, Full),
	__DEFINE_LINK_MODE_PARAMS(56000, LR4, Full),
	__DEFINE_LINK_MODE_PARAMS(25000, CR, Full),
	__DEFINE_LINK_MODE_PARAMS(25000, KR, Full),
	__DEFINE_LINK_MODE_PARAMS(25000, SR, Full),
	__DEFINE_LINK_MODE_PARAMS(50000, CR2, Full),
	__DEFINE_LINK_MODE_PARAMS(50000, KR2, Full),
	__DEFINE_LINK_MODE_PARAMS(100000, KR4, Full),
	__DEFINE_LINK_MODE_PARAMS(100000, SR4, Full),
	__DEFINE_LINK_MODE_PARAMS(100000, CR4, Full),
	__DEFINE_LINK_MODE_PARAMS(100000, LR4_ER4, Full),
	__DEFINE_LINK_MODE_PARAMS(50000, SR2, Full),
	__DEFINE_LINK_MODE_PARAMS(1000, X, Full),
	__DEFINE_LINK_MODE_PARAMS(10000, CR, Full),
	__DEFINE_LINK_MODE_PARAMS(10000, SR, Full),
	__DEFINE_LINK_MODE_PARAMS(10000, LR, Full),
	__DEFINE_LINK_MODE_PARAMS(10000, LRM, Full),
	__DEFINE_LINK_MODE_PARAMS(10000, ER, Full),
	__DEFINE_LINK_MODE_PARAMS(2500, T, Full),
	__DEFINE_LINK_MODE_PARAMS(5000, T, Full),
	__DEFINE_SPECIAL_MODE_PARAMS(FEC_NONE),
	__DEFINE_SPECIAL_MODE_PARAMS(FEC_RS),
	__DEFINE_SPECIAL_MODE_PARAMS(FEC_BASER),
	__DEFINE_LINK_MODE_PARAMS(50000, KR, Full),
	__DEFINE_LINK_MODE_PARAMS(50000, SR, Full),
	__DEFINE_LINK_MODE_PARAMS(50000, CR, Full),
	__DEFINE_LINK_MODE_PARAMS(50000, LR_ER_FR, Full),
	__DEFINE_LINK_MODE_PARAMS(50000, DR, Full),
	__DEFINE_LINK_MODE_PARAMS(100000, KR2, Full),
	__DEFINE_LINK_MODE_PARAMS(100000, SR2, Full),
	__DEFINE_LINK_MODE_PARAMS(100000, CR2, Full),
	__DEFINE_LINK_MODE_PARAMS(100000, LR2_ER2_FR2, Full),
	__DEFINE_LINK_MODE_PARAMS(100000, DR2, Full),
	__DEFINE_LINK_MODE_PARAMS(200000, KR4, Full),
	__DEFINE_LINK_MODE_PARAMS(200000, SR4, Full),
	__DEFINE_LINK_MODE_PARAMS(200000, LR4_ER4_FR4, Full),
	__DEFINE_LINK_MODE_PARAMS(200000, DR4, Full),
	__DEFINE_LINK_MODE_PARAMS(200000, CR4, Full),
	__DEFINE_LINK_MODE_PARAMS(100, T1, Full),
	__DEFINE_LINK_MODE_PARAMS(1000, T1, Full),
	__DEFINE_LINK_MODE_PARAMS(400000, KR8, Full),
	__DEFINE_LINK_MODE_PARAMS(400000, SR8, Full),
	__DEFINE_LINK_MODE_PARAMS(400000, LR8_ER8_FR8, Full),
	__DEFINE_LINK_MODE_PARAMS(400000, DR8, Full),
	__DEFINE_LINK_MODE_PARAMS(400000, CR8, Full),
	__DEFINE_SPECIAL_MODE_PARAMS(FEC_LLRS),
	__DEFINE_LINK_MODE_PARAMS(100000, KR, Full),
	__DEFINE_LINK_MODE_PARAMS(100000, SR, Full),
	__DEFINE_LINK_MODE_PARAMS(100000, LR_ER_FR, Full),
	__DEFINE_LINK_MODE_PARAMS(100000, DR, Full),
	__DEFINE_LINK_MODE_PARAMS(100000, CR, Full),
	__DEFINE_LINK_MODE_PARAMS(200000, KR2, Full),
	__DEFINE_LINK_MODE_PARAMS(200000, SR2, Full),
	__DEFINE_LINK_MODE_PARAMS(200000, LR2_ER2_FR2, Full),
	__DEFINE_LINK_MODE_PARAMS(200000, DR2, Full),
	__DEFINE_LINK_MODE_PARAMS(200000, CR2, Full),
	__DEFINE_LINK_MODE_PARAMS(400000, KR4, Full),
	__DEFINE_LINK_MODE_PARAMS(400000, SR4, Full),
	__DEFINE_LINK_MODE_PARAMS(400000, LR4_ER4_FR4, Full),
	__DEFINE_LINK_MODE_PARAMS(400000, DR4, Full),
	__DEFINE_LINK_MODE_PARAMS(400000, CR4, Full),
	__DEFINE_LINK_MODE_PARAMS(100, FX, Half),
	__DEFINE_LINK_MODE_PARAMS(100, FX, Full),
};
