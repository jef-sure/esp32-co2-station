/*
 * tzones.c
 *
 *  Created on: Mar 26, 2021
 *      Author: anton
 */

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "tzones.h"

const TZName_t TimeZones[] = {
#include "tzones_data.inc"
};

const size_t TimeZonesNumer = sizeof(TimeZones) / sizeof(TZName_t);

const TZName_t *findTZ(const char *name)
{
    const TZName_t *ret = 0;
    for (size_t i = 0; i < sizeof(TimeZones) / sizeof(TZName_t); ++i) {
        if (strcmp(TimeZones[i].name, name) == 0) {
            ret = TimeZones + i;
            break;
        }
    }
    return ret;
}

const TZName_t *findTZbyValue(const char *tzname)
{
    const TZName_t *ret = 0;
    for (size_t i = 0; i < sizeof(TimeZones) / sizeof(TZName_t); ++i) {
        if (strcmp(TimeZones[i].tzname, tzname) == 0) {
            ret = TimeZones + i;
            break;
        }
    }
    return ret;
}

bool setupTZ(const char *name)
{
    const TZName_t *tz = findTZ(name);
    if (tz) {
        setenv("TZ", tz->tzname, 1);
        tzset();
    }
    return tz != 0;
}
