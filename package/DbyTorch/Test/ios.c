/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, UniXNU contributors. All rights reserved. */
/* Host tests of DbyTorchIos.c: the plan the boot arguments make, the last check before launchd, the
 * level of a user client call, and the names the other side of the chain looks for. */
#include <string.h>
#include "check.h"
#include "../DbyTorchIos.h"

int main(void)
{
    DbtIosPlan p, f;
    /* off: silent, nothing made */
    CHECK(dbt_ios_plan(0, 0, &p) == 0 && !p.create && !strcmp(p.why, "off"));
    CHECK(dbt_ios_plan(0, 1, &p) == 0 && !p.create);
    /* refused, logged */
    CHECK(dbt_ios_plan(2, 1, &p) == -1 && !p.create && strstr(p.why, "takes 1"));
    CHECK(dbt_ios_plan(1, 0, &p) == -1 && !p.create && strstr(p.why, "dby-torch=1"));
    CHECK(dbt_ios_plan(1, 2, &p) == -1 && !p.create);   /* a dry run lights nothing, so no button */
    CHECK(dbt_ios_plan(1, 1, 0) == -1);
    /* on */
    CHECK(dbt_ios_plan(1, 1, &p) == 0 && p.create == 1 && !p.why[0]);
    /* the last check: nothing is made without the driver and the plug-in */
    CHECK(dbt_ios_final(&p, 1, 1, &f) == 0 && f.create == 1);
    CHECK(dbt_ios_final(&p, 0, 1, &f) == -1 && !f.create && strstr(f.why, "driver"));
    CHECK(dbt_ios_final(&p, 1, 0, &f) == -1 && !f.create && strstr(f.why, "plug-in"));
    CHECK(dbt_ios_final(&p, 0, 0, &f) == -1 && !f.create);
    CHECK(dbt_ios_final(0, 1, 1, &f) == -1 && dbt_ios_final(&p, 1, 1, 0) == -1);
    /* a plan that was off stays off, without a refusal */
    DbtIosPlan off;
    CHECK(dbt_ios_plan(0, 1, &off) == 0 && dbt_ios_final(&off, 0, 0, &f) == 0 && !f.create);
    /* the user client's scalar */
    CHECK(dbt_ios_level(0) == 0 && dbt_ios_level(1000) == 1000);
    CHECK(dbt_ios_level(1001) == -22 && dbt_ios_level(0x100000000ull) == -22 && dbt_ios_level(~0ull) == -22);
    /* the names the other side looks for */
    CHECK(!strcmp(DBT_IOS_SERVICE, "AppleH6CamIn"));
    CHECK(!strcmp(DBT_IOS_PARENT "/" DBT_IOS_NODE, "/product/camera"));
    /* DBT_IOS_PARENT goes to fromPath with gIODTPlane, so a plane prefix could never be peeled off;
     * DBT_IOS_NODE goes to childFromPath, which walks exactly one component of the plane. */
    CHECK(!strchr(DBT_IOS_PARENT, ':') && DBT_IOS_PARENT[0] == '/');
    CHECK(!strchr(DBT_IOS_NODE, ':') && !strchr(DBT_IOS_NODE, '/'));
    CHECK(!strcmp(DBT_IOS_FLASH, "flash") && !strcmp(DBT_IOS_PROCESS, "mediaserverd"));
    CHECK(!strcmp(DBT_IOS_NODE, "camera") && !strcmp(DBT_IOS_CLIENT, "DbyTorchUserClient"));
    printf("PASS ios: %u checks\n", checks);
    return 0;
}
