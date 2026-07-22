#include "geo_math.h"
#include <math.h>

#define EARTH_R_KM 6371.0088
#define DEG2RAD(d) ((d) * M_PI / 180.0)

double geo_haversine_km(double lat1, double lon1, double lat2, double lon2)
{
    double dlat = DEG2RAD(lat2 - lat1);
    double dlon = DEG2RAD(lon2 - lon1);
    double a = sin(dlat / 2) * sin(dlat / 2) +
               cos(DEG2RAD(lat1)) * cos(DEG2RAD(lat2)) *
               sin(dlon / 2) * sin(dlon / 2);
    return 2 * EARTH_R_KM * atan2(sqrt(a), sqrt(1 - a));
}

double geo_bearing_deg(double lat1, double lon1, double lat2, double lon2)
{
    double p1 = DEG2RAD(lat1), p2 = DEG2RAD(lat2);
    double dl = DEG2RAD(lon2 - lon1);
    double y = sin(dl) * cos(p2);
    double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    double b = atan2(y, x) * 180.0 / M_PI;
    return fmod(b + 360.0, 360.0);
}

void geo_gc_point(double lat1, double lon1, double lat2, double lon2,
                  double f, double *lat, double *lon)
{
    double p1 = DEG2RAD(lat1), l1 = DEG2RAD(lon1);
    double p2 = DEG2RAD(lat2), l2 = DEG2RAD(lon2);
    double d = 2 * asin(sqrt(pow(sin((p2 - p1) / 2), 2) +
                             cos(p1) * cos(p2) * pow(sin((l2 - l1) / 2), 2)));
    if (d < 1e-9) {
        *lat = lat1;
        *lon = lon1;
        return;
    }
    double a = sin((1 - f) * d) / sin(d);
    double b = sin(f * d) / sin(d);
    double x = a * cos(p1) * cos(l1) + b * cos(p2) * cos(l2);
    double y = a * cos(p1) * sin(l1) + b * cos(p2) * sin(l2);
    double z = a * sin(p1) + b * sin(p2);
    *lat = atan2(z, sqrt(x * x + y * y)) * 180.0 / M_PI;
    *lon = atan2(y, x) * 180.0 / M_PI;
}

bool geo_route_plausible(double orig_lat, double orig_lon,
                         double dest_lat, double dest_lon,
                         double cur_lat, double cur_lon)
{
    double direct = geo_haversine_km(orig_lat, orig_lon, dest_lat, dest_lon);
    double detour = geo_haversine_km(orig_lat, orig_lon, cur_lat, cur_lon) +
                    geo_haversine_km(cur_lat, cur_lon, dest_lat, dest_lon);
    /* Real flights fly close to the great circle; allow 30% + 150 km slack
     * for departures, holdings and weather deviations. */
    return detour <= direct * 1.3 + 150.0;
}

double geo_progress(double orig_lat, double orig_lon,
                    double dest_lat, double dest_lon,
                    double cur_lat, double cur_lon)
{
    double flown = geo_haversine_km(orig_lat, orig_lon, cur_lat, cur_lon);
    double remaining = geo_haversine_km(cur_lat, cur_lon, dest_lat, dest_lon);
    double total = flown + remaining;
    if (total < 1.0) {
        return 0.0;
    }
    double p = flown / total;
    return p < 0.0 ? 0.0 : (p > 1.0 ? 1.0 : p);
}
