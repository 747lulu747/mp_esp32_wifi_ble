/**   
 * \par Copyright (C), 2017-2018, MakeBlock
 * \brief   Heard for mb_flame_sensor.c
 * @file    mb_flame_sensor.h
 * @author  fftust
 * @version V1.0.0
 * @date    2017/03/06
 *
 * \par Copyright
 * This software is Copyright (C), 2012-2016, MakeBlock. Use is subject to license \n
 * conditions. The main licensing options available are GPL V2 or Commercial: \n
 *
 * \par Open Source Licensing GPL V2
 * This is the appropriate option if you want to share the source code of your \n
 * application with everyone you distribute it to, and you also want to give them \n
 * the right to share who uses it. If you wish to use this software under Open \n
 * Source Licensing, you must contribute all your source code to the open source \n
 * community in accordance with the GPL Version 2 when your application is \n
 * distributed. See http://www.gnu.org/copyleft/gpl.html
 *
 * \par Description
 * This file is a drive flame_sensor module.
 *
 * \par Method List:
 *
 *
 * \par History:
 * <pre>
 * `<Author>`         `<Time>`        `<Version>`        `<Descr>`
 * fftust              2017/03/06    1.0.0            build the new.
 * </pre>
 *
 */



#ifndef MB_FLAME_SENSOR_H_
#define MB_FLAME_SENSOR_H_

extern const mp_obj_type_t mb_flame_sensor_type;
extern void mb_flame_sensor_value_cmd(uint8_t index, uint8_t port);


#endif /* MB_FLAME_SENSOR_H_*/

