/*
 * Copyright 2017 MapD Technologies, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "DateTruncate.h"
#include "../Shared/unreachable.h"
#include "ExtractFromTime.h"

#ifndef __CUDACC__
#include <glog/logging.h>
#endif

#include <ctime>
#include <iostream>

extern "C" NEVER_INLINE DEVICE int64_t create_epoch(int64_t year) {
  // Note this is not general purpose
  // it has a final assumption that the year being passed can never be a leap
  // year
  // use 2001 epoch time 31 March as start

  int64_t new_time = EPOCH_ADJUSTMENT_DAYS * SECSPERDAY;
  bool forward = true;
  int32_t years_offset = year - ADJUSTED_EPOCH_YEAR;
  // convert year_offset to positive
  if (years_offset < 0) {
    forward = false;
    years_offset = -years_offset;
  }
  // now get number of 400 year cycles in the years_offset;

  int32_t year400 = years_offset / 400;
  int32_t years_remaining = years_offset - (year400 * 400);
  int32_t year100 = years_remaining / 100;
  years_remaining -= year100 * 100;
  int32_t year4 = years_remaining / 4;
  years_remaining -= year4 * 4;

  // get new date I know the final year will never be a leap year
  if (forward) {
    new_time += (year400 * DAYS_PER_400_YEARS + year100 * DAYS_PER_100_YEARS +
                 year4 * DAYS_PER_4_YEARS + years_remaining * DAYS_PER_YEAR -
                 DAYS_IN_JANUARY - DAYS_IN_FEBRUARY) *
                SECSPERDAY;
  } else {
    new_time -= (year400 * DAYS_PER_400_YEARS + year100 * DAYS_PER_100_YEARS +
                 year4 * DAYS_PER_4_YEARS + years_remaining * DAYS_PER_YEAR +
                 // one more day for leap year of 2000 when going backward;
                 1 + DAYS_IN_JANUARY + DAYS_IN_FEBRUARY) *
                SECSPERDAY;
  };

  return new_time;
}

/*
 * @brief support the SQL DATE_TRUNC function
 */
extern "C" NEVER_INLINE DEVICE int64_t DateTruncate(DatetruncField field,
                                                    int64_t timeval) {
  STATIC_QUAL const int32_t month_lengths[2][MONSPERYEAR] = {
      {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
      {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}};

  STATIC_QUAL const uint32_t cumulative_month_epoch_starts[MONSPERYEAR] = {0,
                                                                           2678400,
                                                                           5270400,
                                                                           7948800,
                                                                           10540800,
                                                                           13219200,
                                                                           15897600,
                                                                           18489600,
                                                                           21168000,
                                                                           23760000,
                                                                           26438400,
                                                                           29116800};
  STATIC_QUAL const uint32_t cumulative_quarter_epoch_starts[4] = {
      0, 7776000, 15638400, 23587200};
  STATIC_QUAL const uint32_t cumulative_quarter_epoch_starts_leap_year[4] = {
      0, 7862400, 15724800, 23673600};
  switch (field) {
    case dtNANOSECOND:
    case dtMICROSECOND:
    case dtMILLISECOND:
    case dtSECOND:
      // precision in seconds
      return timeval;
    case dtMINUTE: {
      int64_t ret = (timeval / SECSPERMIN) * SECSPERMIN;
      // in the case of a negative time we still want to push down so need to push
      // one
      // more
      if (ret < 0) {
        {
          ret -= SECSPERMIN;
        }
      }
      return ret;
    }
    case dtHOUR: {
      int64_t ret = (timeval / SECSPERHOUR) * SECSPERHOUR;
      // in the case of a negative time we still want to push down so need to push
      // one
      // more
      if (ret < 0) {
        {
          ret -= SECSPERHOUR;
        }
      }
      return ret;
    }
    case dtQUARTERDAY: {
      int64_t ret = (timeval / SECSPERQUARTERDAY) * SECSPERQUARTERDAY;
      // in the case of a negative time we still want to push down so need to push
      // one
      // more
      if (ret < 0) {
        {
          ret -= SECSPERQUARTERDAY;
        }
      }
      return ret;
    }
    case dtDAY: {
      int64_t ret = (timeval / SECSPERDAY) * SECSPERDAY;
      // in the case of a negative time we still want to push down so need to push
      // one
      // more
      if (ret < 0) {
        {
          ret -= SECSPERDAY;
        }
      }
      return ret;
    }
    case dtWEEK: {
      int64_t day = (timeval / SECSPERDAY) * SECSPERDAY;
      if (day < 0) {
        {
          day -= SECSPERDAY;
        }
      }
      int32_t dow = extract_dow(&day);
      return day - (dow * SECSPERDAY);
    }
    case dtMONTH: {
      if (timeval >= 0L && timeval <= UINT32_MAX - (EPOCH_OFFSET_YEAR_1900)) {
        uint32_t seconds_march_1900 = static_cast<int64_t>(timeval) +
                                      EPOCH_OFFSET_YEAR_1900 -
                                      SECONDS_FROM_JAN_1900_TO_MARCH_1900;
        uint32_t seconds_past_4year_period =
            seconds_march_1900 % SECONDS_PER_4_YEAR_CYCLE;
        uint32_t four_year_period_seconds =
            (seconds_march_1900 / SECONDS_PER_4_YEAR_CYCLE) * SECONDS_PER_4_YEAR_CYCLE;
        uint32_t year_seconds_past_4year_period =
            (seconds_past_4year_period / SECONDS_PER_NON_LEAP_YEAR) *
            SECONDS_PER_NON_LEAP_YEAR;
        if (seconds_past_4year_period >=
            SECONDS_PER_4_YEAR_CYCLE - SECONDS_PER_DAY) {  // if we are in Feb 29th
          year_seconds_past_4year_period -= SECONDS_PER_NON_LEAP_YEAR;
        }
        uint32_t seconds_past_march =
            seconds_past_4year_period - year_seconds_past_4year_period;
        uint32_t month = seconds_past_march /
                         (30 * SECONDS_PER_DAY);  // Will make the correct month either be
                                                  // the guessed month or month before
        month = month <= 11 ? month : 11;
        if (cumulative_month_epoch_starts[month] > seconds_past_march) {
          month--;
        }
        return (four_year_period_seconds + year_seconds_past_4year_period +
                cumulative_month_epoch_starts[month] - EPOCH_OFFSET_YEAR_1900 +
                SECONDS_FROM_JAN_1900_TO_MARCH_1900);
      }
      break;
    }
    case dtQUARTER: {
      if (timeval >= 0L && timeval <= UINT32_MAX - EPOCH_OFFSET_YEAR_1900) {
        uint32_t seconds_1900 = static_cast<int64_t>(timeval) + EPOCH_OFFSET_YEAR_1900;
        uint32_t leap_years = (seconds_1900 - SECONDS_FROM_JAN_1900_TO_MARCH_1900) /
                              SECONDS_PER_4_YEAR_CYCLE;
        uint32_t year =
            (seconds_1900 - leap_years * SECONDS_PER_DAY) / SECONDS_PER_NON_LEAP_YEAR;
        uint32_t base_year_leap_years = (year - 1) / 4;
        uint32_t base_year_seconds =
            year * SECONDS_PER_NON_LEAP_YEAR + base_year_leap_years * SECONDS_PER_DAY;
        bool is_leap_year = year % 4 == 0 && year != 0;
        const uint32_t* quarter_offsets = is_leap_year
                                              ? cumulative_quarter_epoch_starts_leap_year
                                              : cumulative_quarter_epoch_starts;
        uint32_t partial_year_seconds = seconds_1900 % base_year_seconds;
        uint32_t quarter = partial_year_seconds / (90 * SECONDS_PER_DAY);
        quarter = quarter <= 3 ? quarter : 3;
        if (quarter_offsets[quarter] > partial_year_seconds) {
          quarter--;
        }
        return (static_cast<int64_t>(base_year_seconds) + quarter_offsets[quarter] -
                EPOCH_OFFSET_YEAR_1900);
      }
      break;
    }
    case dtYEAR: {
      if (timeval >= 0L && timeval <= UINT32_MAX - EPOCH_OFFSET_YEAR_1900) {
        uint32_t seconds_1900 = static_cast<int64_t>(timeval) + EPOCH_OFFSET_YEAR_1900;
        uint32_t leap_years = (seconds_1900 - SECONDS_FROM_JAN_1900_TO_MARCH_1900) /
                              SECONDS_PER_4_YEAR_CYCLE;
        uint32_t year =
            (seconds_1900 - leap_years * SECONDS_PER_DAY) / SECONDS_PER_NON_LEAP_YEAR;
        uint32_t base_year_leap_years = (year - 1) / 4;
        return (static_cast<int64_t>(year) * SECONDS_PER_NON_LEAP_YEAR +
                base_year_leap_years * SECONDS_PER_DAY - EPOCH_OFFSET_YEAR_1900);
      }
      break;
    }
    default:
      break;
  }

  // use ExtractFromTime functions where available
  // have to do some extra work for these ones
  tm tm_struct;
  gmtime_r_newlib(&timeval, &tm_struct);
  switch (field) {
    case dtMONTH: {
      // clear the time
      int64_t day = static_cast<int64_t>(timeval / SECSPERDAY) * SECSPERDAY;
      if (day < 0) {
        {
          day -= SECSPERDAY;
        }
      }
      // calculate the day of month offset
      int32_t dom = tm_struct.tm_mday;
      return (day - (static_cast<int64_t>(dom - 1) * SECSPERDAY));
    }
    case dtQUARTER: {
      // clear the time
      int64_t day = (static_cast<int64_t>(timeval) / SECSPERDAY) * SECSPERDAY;
      if (day < 0) {
        {
          day -= SECSPERDAY;
        }
      }
      // calculate the day of month offset
      int32_t dom = tm_struct.tm_mday;
      // go to the start of the current month
      day = day - ((dom - 1) * SECSPERDAY);
      // find what month we are
      int32_t mon = tm_struct.tm_mon;
      // find start of quarter
      int32_t start_of_quarter = tm_struct.tm_mon / 3 * 3;
      int32_t year = tm_struct.tm_year + YEAR_BASE;
      // are we in a leap year
      int32_t leap_year = 0;
      // only matters if month is March so save some mod operations
      if (mon == 2) {
        if (((year % 400) == 0) || ((year % 4) == 0 && ((year % 100) != 0))) {
          leap_year = 1;
        }
      }
      // now walk back until at correct quarter start
      for (; mon > start_of_quarter; mon--) {
        day = day - (month_lengths[0 + leap_year][mon - 1] * SECSPERDAY);
      }
      return day;
    }
    case dtYEAR: {
      // clear the time
      int64_t day = (static_cast<int64_t>(timeval) / SECSPERDAY) * SECSPERDAY;
      if (day < 0) {
        {
          day -= SECSPERDAY;
        }
      }
      // calculate the day of year offset
      int32_t doy = tm_struct.tm_yday;
      return day - ((doy)*SECSPERDAY);
    }
    case dtDECADE: {
      int32_t year = tm_struct.tm_year + YEAR_BASE;
      int32_t decade_start = ((year - 1) / 10) * 10 + 1;
      return create_epoch(decade_start);
    }
    case dtCENTURY: {
      int32_t year = tm_struct.tm_year + YEAR_BASE;
      int32_t century_start = ((year - 1) / 100) * 100 + 1;
      return create_epoch(century_start);
    }
    case dtMILLENNIUM: {
      int32_t year = tm_struct.tm_year + YEAR_BASE;
      int32_t millennium_start = ((year - 1) / 1000) * 1000 + 1;
      return create_epoch(millennium_start);
    }
    default:
#ifdef __CUDACC__
      return -1;
#else
      abort();
#endif
  }
}

extern "C" NEVER_INLINE DEVICE int64_t DateTruncateHighPrecision(DatetruncField field,
                                                                 int64_t timeval,
                                                                 const int64_t scale) {
  switch (field) {
    case dtNANOSECOND:
      /* this is the limit of current granularity*/
      // precision in nanoseconds
      return timeval;
    case dtMICROSECOND: {
      // precision in microseconds
      if (scale == MILLISECSPERSEC || scale == MICROSECSPERSEC) {
        return timeval;
      } else if (scale == NANOSECSPERSEC) {
        return (static_cast<int64_t>(timeval) / MILLISECSPERSEC) * MILLISECSPERSEC;
      }
    }
    case dtMILLISECOND: {
      // precision in millisonds
      if (scale == MILLISECSPERSEC) {
        return timeval;
      } else if (scale == MICROSECSPERSEC) {
        return (static_cast<int64_t>(timeval) / MILLISECSPERSEC) * MILLISECSPERSEC;
      } else if (scale == NANOSECSPERSEC) {
        return (static_cast<int64_t>(timeval) / MICROSECSPERSEC) * MICROSECSPERSEC;
      }
    }
    default:
      break;
  }
  const int64_t stimeval = static_cast<int64_t>(timeval) / scale;
  return DateTruncate(field, stimeval) * scale;
}

extern "C" DEVICE int64_t DateTruncateNullable(DatetruncField field,
                                               int64_t timeval,
                                               const int64_t null_val) {
  if (timeval == null_val) {
    return null_val;
  }
  return DateTruncate(field, timeval);
}

extern "C" DEVICE int64_t DateTruncateHighPrecisionNullable(DatetruncField field,
                                                            int64_t timeval,
                                                            const int64_t scale,
                                                            const int64_t null_val) {
  if (timeval == null_val) {
    return null_val;
  }
  return DateTruncateHighPrecision(field, timeval, scale);
}

extern "C" DEVICE int64_t
DateTruncateAlterPrecisionScaleUpNullable(DatetruncField field,
                                          int64_t timeval,
                                          const int64_t scale,
                                          const int64_t null_val) {
  if (timeval == null_val) {
    return null_val;
  }
  return DateTruncateAlterPrecisionScaleUp(field, timeval, scale);
}

extern "C" DEVICE int64_t
DateTruncateAlterPrecisionScaleDownNullable(DatetruncField field,
                                            int64_t timeval,
                                            const int64_t scale,
                                            const int64_t null_val) {
  if (timeval == null_val) {
    return null_val;
  }
  return DateTruncateAlterPrecisionScaleDown(field, timeval, scale);
}

extern "C" DEVICE int64_t DateDiff(const DatetruncField datepart,
                                   int64_t startdate,
                                   int64_t enddate) {
  int64_t res = enddate - startdate;
  switch (datepart) {
    case dtNANOSECOND:
      return res * NANOSECSPERSEC;
    case dtMICROSECOND:
      return res * MICROSECSPERSEC;
    case dtMILLISECOND:
      return res * MILLISECSPERSEC;
    case dtSECOND:
      return res;
    case dtMINUTE:
      return res / SECSPERMIN;
    case dtHOUR:
      return res / SECSPERHOUR;
    case dtQUARTERDAY:
      return res / SECSPERQUARTERDAY;
    case dtDAY:
      return res / SECSPERDAY;
    case dtWEEK:
      return res / (SECSPERDAY * DAYSPERWEEK);
    default:
      break;
  }

  auto future_date = (res > 0);
  auto end = future_date ? enddate : startdate;
  auto start = future_date ? startdate : enddate;
  res = 0;
  int64_t crt = end;
  while (crt > start) {
    const int64_t dt = DateTruncate(datepart, crt);
    if (dt <= start) {
      break;
    }
    ++res;
    crt = dt - 1;
  }
  return future_date ? res : -res;
}

extern "C" DEVICE int64_t DateDiffHighPrecision(const DatetruncField datepart,
                                                int64_t startdate,
                                                int64_t enddate,
                                                const int32_t adj_dimen,
                                                const int64_t adj_scale,
                                                const int64_t sml_scale,
                                                const int64_t scale) {
  /* TODO(wamsi): When adj_dimen is 1 i.e. both precisions are same,
     this code is really not required. We cam direcly do enddate-startdate here.
     Need to address this in refactoring focussed subsequent PR.*/
  int64_t res = (adj_dimen > 0) ? (enddate - (startdate * adj_scale))
                                : ((enddate * adj_scale) - startdate);
  switch (datepart) {
    case dtNANOSECOND:
      // limit of current granularity
      return res;
    case dtMICROSECOND: {
      if (scale == NANOSECSPERSEC) {
        return res / MILLISECSPERSEC;
      } else {
        { return res; }
      }
    }
    case dtMILLISECOND: {
      if (scale == NANOSECSPERSEC) {
        return res / MICROSECSPERSEC;
      } else if (scale == MICROSECSPERSEC) {
        return res / MILLISECSPERSEC;
      } else {
        { return res; }
      }
    }
    default:
      break;
  }
  startdate /= adj_dimen > 0 ? sml_scale : scale;
  enddate /= adj_dimen < 0 ? sml_scale : scale;
  return DateDiff(datepart, startdate, enddate);
}

extern "C" DEVICE int64_t DateDiffNullable(const DatetruncField datepart,
                                           int64_t startdate,
                                           int64_t enddate,
                                           const int64_t null_val) {
  if (startdate == null_val || enddate == null_val) {
    return null_val;
  }
  return DateDiff(datepart, startdate, enddate);
}

extern "C" DEVICE int64_t DateDiffHighPrecisionNullable(const DatetruncField datepart,
                                                        int64_t startdate,
                                                        int64_t enddate,
                                                        const int32_t adj_dimen,
                                                        const int64_t adj_scale,
                                                        const int64_t sml_scale,
                                                        const int64_t scale,
                                                        const int64_t null_val) {
  if (startdate == null_val || enddate == null_val) {
    return null_val;
  }
  return DateDiffHighPrecision(
      datepart, startdate, enddate, adj_dimen, adj_scale, sml_scale, scale);
}
