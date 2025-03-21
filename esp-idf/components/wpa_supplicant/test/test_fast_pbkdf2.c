/*
 * SPDX-FileCopyrightText: 2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include "string.h"
#include <inttypes.h>
#include "unity.h"
#include "utils/common.h"
#include "mbedtls/pkcs5.h"
#include "crypto/sha1.h"

#define MAX_PASSPHRASE_LEN 64
#define MAX_SSID_LEN 32
#define PMK_LEN 32

TEST_CASE("Test pbkdf2", "[crypto-pbkdf2]")
{
	uint8_t PMK[PMK_LEN];
	uint8_t ssid_len;
	uint8_t passphrase_len;
	uint8_t ssid[MAX_SSID_LEN];
	uint8_t passphrase[MAX_PASSPHRASE_LEN];
	uint8_t expected_pmk1[PMK_LEN] =
	{0xe7, 0x90, 0xd0, 0x65, 0x67, 0xf0, 0xbf, 0xca, 0xca, 0x10, 0x88, 0x0b, 0x85, 0xb2, 0x33, 0xe5,
	 0xe1, 0xd5, 0xe5, 0xb8, 0xd0, 0xfd, 0x94, 0x60, 0x56, 0x95, 0x5e, 0x41, 0x5a, 0x7f, 0xfa, 0xfa};

	uint8_t expected_pmk[PMK_LEN];

	/* Compare Fast PBKDF output with expected output*/
	pbkdf2_sha1("espressif", (uint8_t *)"espressif", strlen("espressif"), 4096, PMK, PMK_LEN);
	TEST_ASSERT(memcmp(PMK, expected_pmk1, PMK_LEN) == 0);
	ESP_LOG_BUFFER_HEXDUMP("PMK", PMK, PMK_LEN, ESP_LOG_INFO);
	ESP_LOG_BUFFER_HEXDUMP("expected_pmk", expected_pmk1, PMK_LEN, ESP_LOG_INFO);

	/* Compare fast PBKDF output with mbedtls pbkdf2 function's output */
	pbkdf2_sha1("espressif2", (uint8_t *)"espressif2", strlen("espressif2"), 4096, PMK, PMK_LEN);
	mbedtls_md_context_t sha1_ctx;
	const mbedtls_md_info_t *info_sha1;
	mbedtls_md_init(&sha1_ctx);
	info_sha1 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
	mbedtls_md_setup(&sha1_ctx, info_sha1, 1);
	mbedtls_pkcs5_pbkdf2_hmac(&sha1_ctx, (const unsigned char *) "espressif2",
			strlen("espressif2"), (const unsigned char *)"espressif2", strlen("espressif2"),
			4096, PMK_LEN , expected_pmk);

	TEST_ASSERT(memcmp(PMK, expected_pmk, PMK_LEN) == 0);
	ESP_LOG_BUFFER_HEXDUMP("PMK", PMK, PMK_LEN, ESP_LOG_INFO);
	ESP_LOG_BUFFER_HEXDUMP("expected_pmk", expected_pmk, PMK_LEN, ESP_LOG_INFO);
	mbedtls_md_free(&sha1_ctx);

	/* Calculate PMK using random ssid and passphrase and compare */
	os_memset(ssid, 0, MAX_SSID_LEN);
	os_memset(passphrase, 0, MAX_PASSPHRASE_LEN);
	ssid_len = os_random();
	ssid_len %= MAX_SSID_LEN;

	os_get_random(ssid, ssid_len);

	passphrase_len = os_random();
	passphrase_len %= MAX_PASSPHRASE_LEN;

	os_get_random(passphrase, passphrase_len);
	pbkdf2_sha1((char *)passphrase, ssid, ssid_len, 4096, PMK, PMK_LEN);
	mbedtls_md_context_t sha1_ctx_1;
	const mbedtls_md_info_t *info_sha1_1;
	mbedtls_md_init(&sha1_ctx_1);
	info_sha1_1 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
	mbedtls_md_setup(&sha1_ctx_1, info_sha1_1, 1);
	mbedtls_pkcs5_pbkdf2_hmac(&sha1_ctx_1, (const unsigned char *) passphrase,
			strlen((char *)passphrase), (const unsigned char *)ssid, ssid_len,
			4096, PMK_LEN, expected_pmk1);
	ESP_LOG_BUFFER_HEXDUMP("PMK", PMK, PMK_LEN, ESP_LOG_INFO);
	ESP_LOG_BUFFER_HEXDUMP("expected_pmk", expected_pmk1, PMK_LEN, ESP_LOG_INFO);
	TEST_ASSERT(memcmp(PMK, expected_pmk1, PMK_LEN) == 0);
	mbedtls_md_free(&sha1_ctx_1);
}
