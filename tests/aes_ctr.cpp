/*
  @file
  @brief
    This is an implementation of the AES CTR mode.
    Block size can be chosen in aes.h - available choices are AES128, AES192, AES256.

  @note Input string must be in 16-byte blocks. Space padding if needed.
*/
#include <stdint.h>
#include <stddef.h>
#include <cstring>

constexpr int AES_BITS = 256;    ///< 128, 192, 256
constexpr int NROUND   =         ///< number of rounds in AES Cipher
    (AES_BITS==256)  ? 14 : ((AES_BITS==192) ? 12 : 10);

constexpr int AES_NBLOCK   = 16;                        ///< block length in bytes - AES is 128b block
constexpr int AES_KEY_SZ   = (AES_NBLOCK * (NROUND+1)); ///< number of bytes of round key
constexpr int WORD_PER_KEY = (AES_BITS >> 5);           ///< number of 32-bit words in key

typedef uint8_t  U8;
typedef uint32_t U32;

#define SET(b, a)    (*(U32*)(b) = *(U32*)(a))
#define ROR(t)       ({ U8 t0=t[0]; t[0]=t[1]; t[1]=t[2]; t[2]=t[3]; t[3]=t0; })
#define XOR(c, b, a) (*(U32*)(c) = *(U32*)(b) ^ *(U32*)(a))
#define SUB(t)       ({ for (U8 i=0, *p=(t); i<4; i++, p++) *p=sbox[*p]; })

class AES
{
public:
    AES(U8* key0, U8* iv0);
    void xcrypt(U8* buf, size_t len);

private:
    static constexpr U8 sbox[256] = { ///< lookup table, ROMable
//      0     1     2     3     4     5     6     7      8    9     A      B    C     D     E     F
        0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
        0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
        0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
        0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
        0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
        0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
        0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
        0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
        0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
        0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
        0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
        0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
        0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
        0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
        0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
        0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16
    };
    static constexpr U8 rcon[11] = {
        0x8d, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
    };
    ///                        AES_BITS      256, 192, 128
    U8 rk[AES_KEY_SZ];    ///< RoundKey     [240, 208, 176]
    U8 iv[AES_NBLOCK];    ///< for CTR only [16]
    U8 st[4][4];          ///< morphing states during cipher

    void _expand_key(const U8* key0);
    
    void _update_key(U8 n);
    void _sub_bytes();
    void _shift_rows();
    void _mix_columns();
    void _cipher();
};

constexpr U8 AES::sbox[256];
constexpr U8 AES::rcon[11];

AES::AES(U8 *key0, U8 *iv0) {
    _expand_key(key0);
    memcpy(iv, iv0, AES_NBLOCK);
}
///
///> produces 4 * (NROUND+1) round keys.
///  The round keys are used in each round to decrypt the states.
///
void AES::_expand_key(const U8* key0)
{
    U8 tmp[4]; // Used for the column/row operations
  
    // The first round key is the key itself.
    for (int i = 0; i < WORD_PER_KEY; ++i) {
        SET(&rk[i*4], &key0[i*4]);
    }
    // All other round keys are found from the previous round keys.
    for (int i = WORD_PER_KEY; i < 4 * (NROUND + 1); ++i) {
        SET(tmp, &rk[(i - 1)*4]);
        
        if (i % WORD_PER_KEY == 0) {
            // This function shifts the 4 bytes in a word to the left once.
            // [a0,a1,a2,a3] becomes [a1,a2,a3,a0]
            ROR(tmp);
            SUB(tmp);
            tmp[0] = tmp[0] ^ rcon[i / WORD_PER_KEY];
        }
#if AES_BITS==256
        if (i % WORD_PER_KEY == 4) SUB(tmp);
#endif
        XOR(&rk[i * 4], &rk[(i - WORD_PER_KEY) * 4], tmp);
    }
}
///
///> update state by XOR round keys
///
void AES::_update_key(U8 n)
{
    U8 *k = &rk[n * 16];          ///< round keys in moving window
    for (U8 i=0, *p=(U8*)st; i < 16; ++i) *p++ ^= *k++;
}
///
///> substitutes state by lookup values
///
void AES::_sub_bytes()
{
    for (U8 k=0, *p=(U8*)st; k < 16; p++, k++) *p = sbox[*p];
}
///
///> shifts states rows to the left, each row with different offset.
///
void AES::_shift_rows()
{
    U8 tmp;
    // Rotate first row 1 columns to left  
    tmp      = st[0][1];
    st[0][1] = st[1][1];
    st[1][1] = st[2][1];
    st[2][1] = st[3][1];
    st[3][1] = tmp;

    // Rotate second row 2 columns to left
    tmp      = st[0][2];
    st[0][2] = st[2][2];
    st[2][2] = tmp;

    tmp      = st[1][2];
    st[1][2] = st[3][2];
    st[3][2] = tmp;

    // Rotate third row 3 columns to left
    tmp      = st[0][3];
    st[0][3] = st[3][3];
    st[3][3] = st[2][3];
    st[2][3] = st[1][3];
    st[1][3] = tmp;
}
///
///> mix columns of the state matrix
///
void AES::_mix_columns()
{
    auto xtime = [](U8 v){
        return (v<<1) ^ ((v & 0x80) ? 0x1b : 0);
    };
    for (U8 i = 0, *p=(U8*)st; i < 4; ++i, p+=4) {
        U8 x  = *(p+0) ^ *(p+1) ^ *(p+2) ^ *(p+3);
        U8 t0 = *(p+0);
        *(p+0) ^= xtime(*(p+0) ^ *(p+1)) ^ x;
        *(p+1) ^= xtime(*(p+1) ^ *(p+2)) ^ x;
        *(p+2) ^= xtime(*(p+2) ^ *(p+3)) ^ x;
        *(p+3) ^= xtime(*(p+3) ^ t0)     ^ x;
    }
}
///
///> cipher is the main function that encrypts the PlainText.
///
void AES::_cipher()
{
    // add the 1st round key to the state before starting
    _update_key(0);
    // total NROUND rounds
    // first NROUND-1 rounds are identical
    // These NROUND rounds are executed in the loop below.
    // last one without MixColumns()
    for (U8 n = 1; ; ++n) {
        _sub_bytes();
        _shift_rows();
        if (n == NROUND) break;
        
        _mix_columns();
        _update_key(n);
    }
    // add round key to last round
    _update_key(NROUND);
}
///
///> symmetrical encrypt/decrpt CTR core
///
void AES::xcrypt(U8* buf, size_t len)
{
    int bi = AES_NBLOCK;                      ///< block index
    for (size_t i = 0; i < len; ++i, ++bi) {  /// * loop through PlainText
        if (bi == AES_NBLOCK) {      
            memcpy((U8*)st, iv, AES_NBLOCK);  /// * regen xor compliment in buffer
            _cipher();                        /// * process the block

            for (bi = (AES_NBLOCK - 1); bi >= 0; --bi) {
                if (iv[bi] != 255) {          /// * overflow?
                    iv[bi]++;                 /// * increase counter (in IV)
                    break;
                }
                iv[bi] = 0;                   /// * zero padding
            }
            bi = 0;                           /// * reset block index
        }
        buf[i] ^= ((U8*)st)[bi];              /// XOR buffer context
    }
}

#include <stdio.h>
static int test_ctr()
{
#if AES_BITS==256
    U8 key[32] = {
        0x60, 0x3d, 0xeb, 0x10, 0x15, 0xca, 0x71, 0xbe, 0x2b, 0x73, 0xae, 0xf0, 0x85, 0x7d, 0x77, 0x81,
        0x1f, 0x35, 0x2c, 0x07, 0x3b, 0x61, 0x08, 0xd7, 0x2d, 0x98, 0x10, 0xa3, 0x09, 0x14, 0xdf, 0xf4
    };
    U8 in[64]  = {
        0x60, 0x1e, 0xc3, 0x13, 0x77, 0x57, 0x89, 0xa5, 0xb7, 0xa7, 0xf5, 0x04, 0xbb, 0xf3, 0xd2, 0x28, 
        0xf4, 0x43, 0xe3, 0xca, 0x4d, 0x62, 0xb5, 0x9a, 0xca, 0x84, 0xe9, 0x90, 0xca, 0xca, 0xf5, 0xc5, 
        0x2b, 0x09, 0x30, 0xda, 0xa2, 0x3d, 0xe9, 0x4c, 0xe8, 0x70, 0x17, 0xba, 0x2d, 0x84, 0x98, 0x8d, 
        0xdf, 0xc9, 0xc5, 0x8d, 0xb6, 0x7a, 0xad, 0xa6, 0x13, 0xc2, 0xdd, 0x08, 0x45, 0x79, 0x41, 0xa6
    };
#elif AES_BITS==192
    U8 key[24] = {
        0x8e, 0x73, 0xb0, 0xf7, 0xda, 0x0e, 0x64, 0x52, 0xc8, 0x10, 0xf3, 0x2b, 0x80, 0x90, 0x79, 0xe5, 
        0x62, 0xf8, 0xea, 0xd2, 0x52, 0x2c, 0x6b, 0x7b
    };
    U8 in[64]  = {
        0x1a, 0xbc, 0x93, 0x24, 0x17, 0x52, 0x1c, 0xa2, 0x4f, 0x2b, 0x04, 0x59, 0xfe, 0x7e, 0x6e, 0x0b, 
        0x09, 0x03, 0x39, 0xec, 0x0a, 0xa6, 0xfa, 0xef, 0xd5, 0xcc, 0xc2, 0xc6, 0xf4, 0xce, 0x8e, 0x94, 
        0x1e, 0x36, 0xb2, 0x6b, 0xd1, 0xeb, 0xc6, 0x70, 0xd1, 0xbd, 0x1d, 0x66, 0x56, 0x20, 0xab, 0xf7, 
        0x4f, 0x78, 0xa7, 0xf6, 0xd2, 0x98, 0x09, 0x58, 0x5a, 0x97, 0xda, 0xec, 0x58, 0xc6, 0xb0, 0x50
    };
#elif AES_BITS==128
    U8 key[16] = {
        0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c
    };
    U8 in[64]  = {
        0x87, 0x4d, 0x61, 0x91, 0xb6, 0x20, 0xe3, 0x26, 0x1b, 0xef, 0x68, 0x64, 0x99, 0x0d, 0xb6, 0xce,
        0x98, 0x06, 0xf6, 0x6b, 0x79, 0x70, 0xfd, 0xff, 0x86, 0x17, 0x18, 0x7b, 0xb9, 0xff, 0xfd, 0xff,
        0x5a, 0xe4, 0xdf, 0x3e, 0xdb, 0xd5, 0xd3, 0x5e, 0x5b, 0x4f, 0x09, 0x02, 0x0d, 0xb0, 0x3e, 0xab,
        0x1e, 0x03, 0x1d, 0xda, 0x2f, 0xbe, 0x03, 0xd1, 0x79, 0x21, 0x70, 0xa0, 0xf3, 0x00, 0x9c, 0xee
    };
#endif
    U8 iv[16]  = {
        0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff
    };
    U8 out[64] = {
        0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
        0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
        0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef,
        0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10
    };
    struct AES ctx(key, iv);
    
    ctx.xcrypt(in, 64);  // both encrypt/decrypt use the same function
  
    return memcmp((char*)out, (char*)in, 64);
}

int main(void)
{
    printf("AES_BITS=%d, AES_KEY_SZ=%d, WORD_PER_KEY=%d\n",
           AES_BITS, AES_KEY_SZ, WORD_PER_KEY);
    printf("encrypt=%s\n", test_ctr() ? "Failed" : "OK");
    printf("decrypt=%s\n", test_ctr() ? "Failed" : "OK");
}


