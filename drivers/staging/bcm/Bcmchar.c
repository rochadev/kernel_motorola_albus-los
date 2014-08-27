#include <linux/fs.h>

#include "headers.h"

static int bcm_handle_nvm_read_cmd(struct bcm_mini_adapter *ad,
				   PUCHAR read_data,
				   struct bcm_nvm_readwrite *nvm_rw)
{
	INT status = STATUS_FAILURE;

	down(&ad->NVMRdmWrmLock);

	if ((ad->IdleMode == TRUE) || (ad->bShutStatus == TRUE) ||
			(ad->bPreparingForLowPowerMode == TRUE)) {

		BCM_DEBUG_PRINT(ad,
			DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			"Device is in Idle/Shutdown Mode\n");
		up(&ad->NVMRdmWrmLock);
		kfree(read_data);
		return -EACCES;
	}

	status = BeceemNVMRead(ad, (PUINT)read_data,
			       nvm_rw->uiOffset,
			       nvm_rw->uiNumBytes);
	up(&ad->NVMRdmWrmLock);

	if (status != STATUS_SUCCESS) {
		kfree(read_data);
		return status;
	}

	if (copy_to_user(nvm_rw->pBuffer, read_data, nvm_rw->uiNumBytes)) {
		kfree(read_data);
		return -EFAULT;
	}

	return STATUS_SUCCESS;
}

static int handle_flash2x_adapter(struct bcm_mini_adapter *ad,
				  PUCHAR read_data,
				  struct bcm_nvm_readwrite *nvm_rw)
{
	/*
	 * New Requirement:-
	 * DSD section updation will be allowed in two case:-
	 * 1.  if DSD sig is present in DSD header means dongle
	 * is ok and updation is fruitfull
	 * 2.  if point 1 failes then user buff should have
	 * DSD sig. this point ensures that if dongle is
	 * corrupted then user space program first modify
	 * the DSD header with valid DSD sig so that this
	 * as well as further write may be worthwhile.
	 *
	 * This restriction has been put assuming that
	 * if DSD sig is corrupted, DSD data won't be
	 * considered valid.
	 */
	INT status;
	ULONG dsd_magic_num_in_usr_buff = 0;

	status = BcmFlash2xCorruptSig(ad, ad->eActiveDSD);
	if (status == STATUS_SUCCESS)
		return STATUS_SUCCESS;

	if (((nvm_rw->uiOffset + nvm_rw->uiNumBytes) !=
			ad->uiNVMDSDSize) ||
			(nvm_rw->uiNumBytes < SIGNATURE_SIZE)) {

		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
				"DSD Sig is present neither in Flash nor User provided Input..");
		up(&ad->NVMRdmWrmLock);
		kfree(read_data);
		return status;
	}

	dsd_magic_num_in_usr_buff =
		ntohl(*(PUINT)(read_data + nvm_rw->uiNumBytes -
		      SIGNATURE_SIZE));
	if (dsd_magic_num_in_usr_buff != DSD_IMAGE_MAGIC_NUMBER) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
				"DSD Sig is present neither in Flash nor User provided Input..");
		up(&ad->NVMRdmWrmLock);
		kfree(read_data);
		return status;
	}

	return STATUS_SUCCESS;
}

/***************************************************************
* Function	  - bcm_char_open()
*
* Description - This is the "open" entry point for the character
*				driver.
*
* Parameters  - inode: Pointer to the Inode structure of char device
*				filp : File pointer of the char device
*
* Returns	  - Zero(Success)
****************************************************************/

static int bcm_char_open(struct inode *inode, struct file *filp)
{
	struct bcm_mini_adapter *ad = NULL;
	struct bcm_tarang_data *tarang = NULL;

	ad = GET_BCM_ADAPTER(gblpnetdev);
	tarang = kzalloc(sizeof(struct bcm_tarang_data), GFP_KERNEL);
	if (!tarang)
		return -ENOMEM;

	tarang->Adapter = ad;
	tarang->RxCntrlMsgBitMask = 0xFFFFFFFF & ~(1 << 0xB);

	down(&ad->RxAppControlQueuelock);
	tarang->next = ad->pTarangs;
	ad->pTarangs = tarang;
	up(&ad->RxAppControlQueuelock);

	/* Store the Adapter structure */
	filp->private_data = tarang;

	/* Start Queuing the control response Packets */
	atomic_inc(&ad->ApplicationRunning);

	nonseekable_open(inode, filp);
	return 0;
}

static int bcm_char_release(struct inode *inode, struct file *filp)
{
	struct bcm_tarang_data *tarang, *tmp, *ptmp;
	struct bcm_mini_adapter *ad = NULL;
	struct sk_buff *pkt, *npkt;

	tarang = (struct bcm_tarang_data *)filp->private_data;

	if (tarang == NULL)
		return 0;

	ad = tarang->Adapter;

	down(&ad->RxAppControlQueuelock);

	tmp = ad->pTarangs;
	for (ptmp = NULL; tmp; ptmp = tmp, tmp = tmp->next) {
		if (tmp == tarang)
			break;
	}

	if (tmp) {
		if (!ptmp)
			ad->pTarangs = tmp->next;
		else
			ptmp->next = tmp->next;
	} else {
		up(&ad->RxAppControlQueuelock);
		return 0;
	}

	pkt = tarang->RxAppControlHead;
	while (pkt) {
		npkt = pkt->next;
		kfree_skb(pkt);
		pkt = npkt;
	}

	up(&ad->RxAppControlQueuelock);

	/* Stop Queuing the control response Packets */
	atomic_dec(&ad->ApplicationRunning);

	kfree(tarang);

	/* remove this filp from the asynchronously notified filp's */
	filp->private_data = NULL;
	return 0;
}

static ssize_t bcm_char_read(struct file *filp,
			     char __user *buf,
			     size_t size,
			     loff_t *f_pos)
{
	struct bcm_tarang_data *tarang = filp->private_data;
	struct bcm_mini_adapter *ad = tarang->Adapter;
	struct sk_buff *packet = NULL;
	ssize_t pkt_len = 0;
	int wait_ret_val = 0;
	unsigned long ret = 0;

	wait_ret_val = wait_event_interruptible(
				ad->process_read_wait_queue,
				(tarang->RxAppControlHead ||
				ad->device_removed));

	if ((wait_ret_val == -ERESTARTSYS)) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
				"Exiting as i've been asked to exit!!!\n");
		return wait_ret_val;
	}

	if (ad->device_removed) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
				"Device Removed... Killing the Apps...\n");
		return -ENODEV;
	}

	if (false == ad->fw_download_done)
		return -EACCES;

	down(&ad->RxAppControlQueuelock);

	if (tarang->RxAppControlHead) {
		packet = tarang->RxAppControlHead;
		DEQUEUEPACKET(tarang->RxAppControlHead,
			      tarang->RxAppControlTail);
		tarang->AppCtrlQueueLen--;
	}

	up(&ad->RxAppControlQueuelock);

	if (packet) {
		pkt_len = packet->len;
		ret = copy_to_user(buf, packet->data,
				   min_t(size_t, pkt_len, size));
		if (ret) {
			dev_kfree_skb(packet);
			BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
					"Returning from copy to user failure\n");
			return -EFAULT;
		}
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
				"Read %zd Bytes From Adapter packet = %p by process %d!\n",
				pkt_len, packet, current->pid);
		dev_kfree_skb(packet);
	}

	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL, "<\n");
	return pkt_len;
}

static int bcm_char_ioctl_reg_read_private(void __user *argp,
					   struct bcm_mini_adapter *ad)
{
	struct bcm_rdm_buffer rdm_buff = {0};
	struct bcm_ioctl_buffer io_buff;
	PCHAR temp_buff;
	INT status = STATUS_FAILURE;
	UINT buff_len;
	u16 temp_value;
	int bytes;

	/* Copy Ioctl Buffer structure */
	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer)))
		return -EFAULT;

	if (io_buff.InputLength > sizeof(rdm_buff))
		return -EINVAL;

	if (copy_from_user(&rdm_buff, io_buff.InputBuffer,
		io_buff.InputLength))
		return -EFAULT;

	if (io_buff.OutputLength > USHRT_MAX ||
		io_buff.OutputLength == 0) {
		return -EINVAL;
	}

	buff_len = io_buff.OutputLength;
	temp_value = 4 - (buff_len % 4);
	buff_len += temp_value % 4;

	temp_buff = kmalloc(buff_len, GFP_KERNEL);
	if (!temp_buff)
		return -ENOMEM;

	bytes = rdmalt(ad, (UINT)rdm_buff.Register,
			(PUINT)temp_buff, buff_len);
	if (bytes > 0) {
		status = STATUS_SUCCESS;
		if (copy_to_user(io_buff.OutputBuffer, temp_buff, bytes)) {
			kfree(temp_buff);
			return -EFAULT;
		}
	} else {
		status = bytes;
	}

	kfree(temp_buff);
	return status;
}

static int bcm_char_ioctl_reg_write_private(void __user *argp,
					    struct bcm_mini_adapter *ad)
{
	struct bcm_wrm_buffer wrm_buff = {0};
	struct bcm_ioctl_buffer io_buff;
	UINT tmp = 0;
	INT status;

	/* Copy Ioctl Buffer structure */

	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer)))
		return -EFAULT;

	if (io_buff.InputLength > sizeof(wrm_buff))
		return -EINVAL;

	/* Get WrmBuffer structure */
	if (copy_from_user(&wrm_buff, io_buff.InputBuffer,
		io_buff.InputLength))
		return -EFAULT;

	tmp = wrm_buff.Register & EEPROM_REJECT_MASK;
	if (!((ad->pstargetparams->m_u32Customize) & VSG_MODE) &&
		((tmp == EEPROM_REJECT_REG_1) ||
			(tmp == EEPROM_REJECT_REG_2) ||
			(tmp == EEPROM_REJECT_REG_3) ||
			(tmp == EEPROM_REJECT_REG_4))) {

		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"EEPROM Access Denied, not in VSG Mode\n");
		return -EFAULT;
	}

	status = wrmalt(ad, (UINT)wrm_buff.Register,
			(PUINT)wrm_buff.Data, sizeof(ULONG));

	if (status == STATUS_SUCCESS) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG,
				DBG_LVL_ALL, "WRM Done\n");
	} else {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG,
				DBG_LVL_ALL, "WRM Failed\n");
		status = -EFAULT;
	}
	return status;
}

static int bcm_char_ioctl_eeprom_reg_read(void __user *argp,
					  struct bcm_mini_adapter *ad)
{
	struct bcm_rdm_buffer rdm_buff = {0};
	struct bcm_ioctl_buffer io_buff;
	PCHAR temp_buff = NULL;
	UINT tmp = 0;
	INT status;
	int bytes;

	if ((ad->IdleMode == TRUE) ||
		(ad->bShutStatus == TRUE) ||
		(ad->bPreparingForLowPowerMode == TRUE)) {

		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"Device in Idle Mode, Blocking Rdms\n");
		return -EACCES;
	}

	/* Copy Ioctl Buffer structure */
	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer)))
		return -EFAULT;

	if (io_buff.InputLength > sizeof(rdm_buff))
		return -EINVAL;

	if (copy_from_user(&rdm_buff, io_buff.InputBuffer,
		io_buff.InputLength))
		return -EFAULT;

	if (io_buff.OutputLength > USHRT_MAX ||
		io_buff.OutputLength == 0) {
		return -EINVAL;
	}

	temp_buff = kmalloc(io_buff.OutputLength, GFP_KERNEL);
	if (!temp_buff)
		return STATUS_FAILURE;

	if ((((ULONG)rdm_buff.Register & 0x0F000000) != 0x0F000000) ||
		((ULONG)rdm_buff.Register & 0x3)) {

		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"RDM Done On invalid Address : %x Access Denied.\n",
				(int)rdm_buff.Register);

		kfree(temp_buff);
		return -EINVAL;
	}

	tmp = rdm_buff.Register & EEPROM_REJECT_MASK;
	bytes = rdmaltWithLock(ad, (UINT)rdm_buff.Register,
			       (PUINT)temp_buff, io_buff.OutputLength);

	if (bytes > 0) {
		status = STATUS_SUCCESS;
		if (copy_to_user(io_buff.OutputBuffer, temp_buff, bytes)) {
			kfree(temp_buff);
			return -EFAULT;
		}
	} else {
		status = bytes;
	}

	kfree(temp_buff);
	return status;
}

static int bcm_char_ioctl_eeprom_reg_write(void __user *argp,
					   struct bcm_mini_adapter *ad,
					   UINT cmd)
{
	struct bcm_wrm_buffer wrm_buff = {0};
	struct bcm_ioctl_buffer io_buff;
	UINT tmp = 0;
	INT status;

	if ((ad->IdleMode == TRUE) ||
		(ad->bShutStatus == TRUE) ||
		(ad->bPreparingForLowPowerMode == TRUE)) {

		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"Device in Idle Mode, Blocking Wrms\n");
		return -EACCES;
	}

	/* Copy Ioctl Buffer structure */
	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer)))
		return -EFAULT;

	if (io_buff.InputLength > sizeof(wrm_buff))
		return -EINVAL;

	/* Get WrmBuffer structure */
	if (copy_from_user(&wrm_buff, io_buff.InputBuffer,
		io_buff.InputLength))
		return -EFAULT;

	if ((((ULONG)wrm_buff.Register & 0x0F000000) != 0x0F000000) ||
		((ULONG)wrm_buff.Register & 0x3)) {

		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"WRM Done On invalid Address : %x Access Denied.\n",
				(int)wrm_buff.Register);
		return -EINVAL;
	}

	tmp = wrm_buff.Register & EEPROM_REJECT_MASK;
	if (!((ad->pstargetparams->m_u32Customize) & VSG_MODE) &&
			((tmp == EEPROM_REJECT_REG_1) ||
			(tmp == EEPROM_REJECT_REG_2) ||
			(tmp == EEPROM_REJECT_REG_3) ||
			(tmp == EEPROM_REJECT_REG_4)) &&
			(cmd == IOCTL_BCM_REGISTER_WRITE)) {

			BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
					"EEPROM Access Denied, not in VSG Mode\n");
			return -EFAULT;
	}

	status = wrmaltWithLock(ad, (UINT)wrm_buff.Register,
				(PUINT)wrm_buff.Data,
				wrm_buff.Length);

	if (status == STATUS_SUCCESS) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, OSAL_DBG,
				DBG_LVL_ALL, "WRM Done\n");
	} else {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG,
				DBG_LVL_ALL, "WRM Failed\n");
		status = -EFAULT;
	}
	return status;
}

static int bcm_char_ioctl_gpio_set_request(void __user *argp,
					   struct bcm_mini_adapter *ad)
{
	struct bcm_gpio_info gpio_info = {0};
	struct bcm_ioctl_buffer io_buff;
	UCHAR reset_val[4];
	UINT value = 0;
	UINT bit = 0;
	UINT operation = 0;
	INT status;
	int bytes;

	if ((ad->IdleMode == TRUE) ||
		(ad->bShutStatus == TRUE) ||
		(ad->bPreparingForLowPowerMode == TRUE)) {

		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG,
				DBG_LVL_ALL,
				"GPIO Can't be set/clear in Low power Mode");
		return -EACCES;
	}

	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer)))
		return -EFAULT;

	if (io_buff.InputLength > sizeof(gpio_info))
		return -EINVAL;

	if (copy_from_user(&gpio_info, io_buff.InputBuffer,
			   io_buff.InputLength))
		return -EFAULT;

	bit  = gpio_info.uiGpioNumber;
	operation = gpio_info.uiGpioValue;
	value = (1<<bit);

	if (IsReqGpioIsLedInNVM(ad, value) == false) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG,
				DBG_LVL_ALL,
				"Sorry, Requested GPIO<0x%X> is not correspond to LED !!!",
				value);
		return -EINVAL;
	}

	/* Set - setting 1 */
	if (operation) {
		/* Set the gpio output register */
		status = wrmaltWithLock(ad,
					BCM_GPIO_OUTPUT_SET_REG,
					(PUINT)(&value), sizeof(UINT));

		if (status == STATUS_SUCCESS) {
			BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS,
					OSAL_DBG, DBG_LVL_ALL,
					"Set the GPIO bit\n");
		} else {
			BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS,
					OSAL_DBG, DBG_LVL_ALL,
					"Failed to set the %dth GPIO\n",
					bit);
			return status;
		}
	} else {
		/* Set the gpio output register */
		status = wrmaltWithLock(ad,
					BCM_GPIO_OUTPUT_CLR_REG,
					(PUINT)(&value), sizeof(UINT));

		if (status == STATUS_SUCCESS) {
			BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS,
					OSAL_DBG, DBG_LVL_ALL,
					"Set the GPIO bit\n");
		} else {
			BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS,
					OSAL_DBG, DBG_LVL_ALL,
					"Failed to clear the %dth GPIO\n",
					bit);
			return status;
		}
	}

	bytes = rdmaltWithLock(ad, (UINT)GPIO_MODE_REGISTER,
			       (PUINT)reset_val, sizeof(UINT));
	if (bytes < 0) {
		status = bytes;
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
				"GPIO_MODE_REGISTER read failed");
		return status;
	} else {
		status = STATUS_SUCCESS;
	}

	/* Set the gpio mode register to output */
	*(UINT *)reset_val |= (1<<bit);
	status = wrmaltWithLock(ad, GPIO_MODE_REGISTER,
				(PUINT)reset_val, sizeof(UINT));

	if (status == STATUS_SUCCESS) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG,
				DBG_LVL_ALL,
				"Set the GPIO to output Mode\n");
	} else {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG,
				DBG_LVL_ALL,
				"Failed to put GPIO in Output Mode\n");
	}

	return status;
}

static int bcm_char_ioctl_led_thread_state_change_req(void __user *argp,
		struct bcm_mini_adapter *ad)
{
	struct bcm_user_thread_req thread_req = {0};
	struct bcm_ioctl_buffer io_buff;

	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			"User made LED thread InActive");

	if ((ad->IdleMode == TRUE) ||
		(ad->bShutStatus == TRUE) ||
		(ad->bPreparingForLowPowerMode == TRUE)) {

		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG,
				DBG_LVL_ALL,
				"GPIO Can't be set/clear in Low power Mode");
		return -EACCES;
	}

	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer)))
		return -EFAULT;

	if (io_buff.InputLength > sizeof(thread_req))
		return -EINVAL;

	if (copy_from_user(&thread_req, io_buff.InputBuffer,
			   io_buff.InputLength))
		return -EFAULT;

	/* if LED thread is running(Actively or Inactively)
	 * set it state to make inactive
	 */
	if (ad->LEDInfo.led_thread_running) {
		if (thread_req.ThreadState == LED_THREAD_ACTIVATION_REQ) {
			BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS,
					OSAL_DBG, DBG_LVL_ALL,
					"Activating thread req");
			ad->DriverState = LED_THREAD_ACTIVE;
		} else {
			BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS,
					OSAL_DBG, DBG_LVL_ALL,
					"DeActivating Thread req.....");
			ad->DriverState = LED_THREAD_INACTIVE;
		}

		/* signal thread. */
		wake_up(&ad->LEDInfo.notify_led_event);
	}
	return STATUS_SUCCESS;
}

static int bcm_char_ioctl_gpio_status_request(void __user *argp,
					      struct bcm_mini_adapter *ad)
{
	struct bcm_gpio_info gpio_info = {0};
	struct bcm_ioctl_buffer io_buff;
	ULONG bit = 0;
	UCHAR read[4];
	INT Status;
	int bytes;

	if ((ad->IdleMode == TRUE) ||
		(ad->bShutStatus == TRUE) ||
		(ad->bPreparingForLowPowerMode == TRUE))
		return -EACCES;

	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer)))
		return -EFAULT;

	if (io_buff.InputLength > sizeof(gpio_info))
		return -EINVAL;

	if (copy_from_user(&gpio_info, io_buff.InputBuffer,
		io_buff.InputLength))
		return -EFAULT;

	bit = gpio_info.uiGpioNumber;

	/* Set the gpio output register */
	bytes = rdmaltWithLock(ad, (UINT)GPIO_PIN_STATE_REGISTER,
				(PUINT)read, sizeof(UINT));

	if (bytes < 0) {
		Status = bytes;
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"RDM Failed\n");
		return Status;
	} else {
		Status = STATUS_SUCCESS;
	}
	return Status;
}

static int bcm_char_ioctl_gpio_multi_request(void __user *argp,
					     struct bcm_mini_adapter *ad)
{
	struct bcm_gpio_multi_info gpio_multi_info[MAX_IDX];
	struct bcm_gpio_multi_info *pgpio_multi_info =
		(struct bcm_gpio_multi_info *)gpio_multi_info;
	struct bcm_ioctl_buffer io_buff;
	UCHAR ucResetValue[4];
	INT Status = STATUS_FAILURE;
	int bytes;

	memset(pgpio_multi_info, 0,
	       MAX_IDX * sizeof(struct bcm_gpio_multi_info));

	if ((ad->IdleMode == TRUE) ||
		(ad->bShutStatus == TRUE) ||
		(ad->bPreparingForLowPowerMode == TRUE))
		return -EINVAL;

	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer)))
		return -EFAULT;

	if (io_buff.InputLength > sizeof(gpio_multi_info))
		return -EINVAL;
	if (io_buff.OutputLength > sizeof(gpio_multi_info))
		io_buff.OutputLength = sizeof(gpio_multi_info);

	if (copy_from_user(&gpio_multi_info, io_buff.InputBuffer,
			   io_buff.InputLength))
		return -EFAULT;

	if (IsReqGpioIsLedInNVM(ad, pgpio_multi_info[WIMAX_IDX].uiGPIOMask)
			== false) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG,
				DBG_LVL_ALL,
				"Sorry, Requested GPIO<0x%X> is not correspond to NVM LED bit map<0x%X>!!!",
				pgpio_multi_info[WIMAX_IDX].uiGPIOMask,
				ad->gpioBitMap);
		return -EINVAL;
	}

	/* Set the gpio output register */
	if ((pgpio_multi_info[WIMAX_IDX].uiGPIOMask) &
		(pgpio_multi_info[WIMAX_IDX].uiGPIOCommand)) {
		/* Set 1's in GPIO OUTPUT REGISTER */
		*(UINT *)ucResetValue = pgpio_multi_info[WIMAX_IDX].uiGPIOMask &
			pgpio_multi_info[WIMAX_IDX].uiGPIOCommand &
			pgpio_multi_info[WIMAX_IDX].uiGPIOValue;

		if (*(UINT *) ucResetValue)
			Status = wrmaltWithLock(ad,
				BCM_GPIO_OUTPUT_SET_REG,
				(PUINT)ucResetValue, sizeof(ULONG));

		if (Status != STATUS_SUCCESS) {
			BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"WRM to BCM_GPIO_OUTPUT_SET_REG Failed.");
			return Status;
		}

		/* Clear to 0's in GPIO OUTPUT REGISTER */
		*(UINT *)ucResetValue =
			(pgpio_multi_info[WIMAX_IDX].uiGPIOMask &
			pgpio_multi_info[WIMAX_IDX].uiGPIOCommand &
			(~(pgpio_multi_info[WIMAX_IDX].uiGPIOValue)));

		if (*(UINT *) ucResetValue)
			Status = wrmaltWithLock(ad,
				BCM_GPIO_OUTPUT_CLR_REG, (PUINT)ucResetValue,
				sizeof(ULONG));

		if (Status != STATUS_SUCCESS) {
			BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
					"WRM to BCM_GPIO_OUTPUT_CLR_REG Failed.");
			return Status;
		}
	}

	if (pgpio_multi_info[WIMAX_IDX].uiGPIOMask) {
		bytes = rdmaltWithLock(ad, (UINT)GPIO_PIN_STATE_REGISTER,
				       (PUINT)ucResetValue, sizeof(UINT));

		if (bytes < 0) {
			Status = bytes;
			BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
					"RDM to GPIO_PIN_STATE_REGISTER Failed.");
			return Status;
		} else {
			Status = STATUS_SUCCESS;
		}

		pgpio_multi_info[WIMAX_IDX].uiGPIOValue =
			(*(UINT *)ucResetValue &
			pgpio_multi_info[WIMAX_IDX].uiGPIOMask);
	}

	Status = copy_to_user(io_buff.OutputBuffer, &gpio_multi_info,
		io_buff.OutputLength);
	if (Status) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
			"Failed while copying Content to IOBufer for user space err:%d",
			Status);
		return -EFAULT;
	}
	return Status;
}

static int bcm_char_ioctl_gpio_mode_request(void __user *argp,
					    struct bcm_mini_adapter *ad)
{
	struct bcm_gpio_multi_mode gpio_multi_mode[MAX_IDX];
	struct bcm_gpio_multi_mode *pgpio_multi_mode =
		(struct bcm_gpio_multi_mode *)gpio_multi_mode;
	struct bcm_ioctl_buffer io_buff;
	UCHAR ucResetValue[4];
	INT Status;
	int bytes;

	if ((ad->IdleMode == TRUE) ||
		(ad->bShutStatus == TRUE) ||
		(ad->bPreparingForLowPowerMode == TRUE))
		return -EINVAL;

	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer)))
		return -EFAULT;

	if (io_buff.InputLength > sizeof(gpio_multi_mode))
		return -EINVAL;
	if (io_buff.OutputLength > sizeof(gpio_multi_mode))
		io_buff.OutputLength = sizeof(gpio_multi_mode);

	if (copy_from_user(&gpio_multi_mode, io_buff.InputBuffer,
		io_buff.InputLength))
		return -EFAULT;

	bytes = rdmaltWithLock(ad, (UINT)GPIO_MODE_REGISTER,
		(PUINT)ucResetValue, sizeof(UINT));

	if (bytes < 0) {
		Status = bytes;
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
			"Read of GPIO_MODE_REGISTER failed");
		return Status;
	} else {
		Status = STATUS_SUCCESS;
	}

	/* Validating the request */
	if (IsReqGpioIsLedInNVM(ad, pgpio_multi_mode[WIMAX_IDX].uiGPIOMask)
			== false) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
				"Sorry, Requested GPIO<0x%X> is not correspond to NVM LED bit map<0x%X>!!!",
				pgpio_multi_mode[WIMAX_IDX].uiGPIOMask,
				ad->gpioBitMap);
		return -EINVAL;
	}

	if (pgpio_multi_mode[WIMAX_IDX].uiGPIOMask) {
		/* write all OUT's (1's) */
		*(UINT *) ucResetValue |=
			(pgpio_multi_mode[WIMAX_IDX].uiGPIOMode &
					pgpio_multi_mode[WIMAX_IDX].uiGPIOMask);

		/* write all IN's (0's) */
		*(UINT *) ucResetValue &=
			~((~pgpio_multi_mode[WIMAX_IDX].uiGPIOMode) &
					pgpio_multi_mode[WIMAX_IDX].uiGPIOMask);

		/* Currently implemented return the modes of all GPIO's
		 * else needs to bit AND with  mask
		 */
		pgpio_multi_mode[WIMAX_IDX].uiGPIOMode = *(UINT *)ucResetValue;

		Status = wrmaltWithLock(ad, GPIO_MODE_REGISTER,
			(PUINT)ucResetValue, sizeof(ULONG));
		if (Status == STATUS_SUCCESS) {
			BCM_DEBUG_PRINT(ad,
				DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
				"WRM to GPIO_MODE_REGISTER Done");
		} else {
			BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
					"WRM to GPIO_MODE_REGISTER Failed");
			return -EFAULT;
		}
	} else {
		/* if uiGPIOMask is 0 then return mode register configuration */
		pgpio_multi_mode[WIMAX_IDX].uiGPIOMode = *(UINT *)ucResetValue;
	}

	Status = copy_to_user(io_buff.OutputBuffer, &gpio_multi_mode,
		io_buff.OutputLength);
	if (Status) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
			"Failed while copying Content to IOBufer for user space err:%d",
			Status);
		return -EFAULT;
	}
	return Status;
}

static int bcm_char_ioctl_misc_request(void __user *argp,
				       struct bcm_mini_adapter *ad)
{
	struct bcm_ioctl_buffer io_buff;
	PVOID pvBuffer = NULL;
	INT Status;

	/* Copy Ioctl Buffer structure */
	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer)))
		return -EFAULT;

	if (io_buff.InputLength < sizeof(struct bcm_link_request))
		return -EINVAL;

	if (io_buff.InputLength > MAX_CNTL_PKT_SIZE)
		return -EINVAL;

	pvBuffer = memdup_user(io_buff.InputBuffer,
			       io_buff.InputLength);
	if (IS_ERR(pvBuffer))
		return PTR_ERR(pvBuffer);

	down(&ad->LowPowerModeSync);
	Status = wait_event_interruptible_timeout(
			ad->lowpower_mode_wait_queue,
			!ad->bPreparingForLowPowerMode,
			(1 * HZ));

	if (Status == -ERESTARTSYS)
		goto cntrlEnd;

	if (ad->bPreparingForLowPowerMode) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
				"Preparing Idle Mode is still True - Hence Rejecting control message\n");
		Status = STATUS_FAILURE;
		goto cntrlEnd;
	}
	Status = CopyBufferToControlPacket(ad, (PVOID)pvBuffer);

cntrlEnd:
	up(&ad->LowPowerModeSync);
	kfree(pvBuffer);
	return Status;
}

static int bcm_char_ioctl_buffer_download_start(
		struct bcm_mini_adapter *ad)
{
	INT Status;

	if (down_trylock(&ad->NVMRdmWrmLock)) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
				"IOCTL_BCM_CHIP_RESET not allowed as EEPROM Read/Write is in progress\n");
		return -EACCES;
	}

	BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
			"Starting the firmware download PID =0x%x!!!!\n",
			current->pid);

	if (down_trylock(&ad->fw_download_sema))
		return -EBUSY;

	ad->bBinDownloaded = false;
	ad->fw_download_process_pid = current->pid;
	ad->bCfgDownloaded = false;
	ad->fw_download_done = false;
	netif_carrier_off(ad->dev);
	netif_stop_queue(ad->dev);
	Status = reset_card_proc(ad);
	if (Status) {
		pr_err(PFX "%s: reset_card_proc Failed!\n", ad->dev->name);
		up(&ad->fw_download_sema);
		up(&ad->NVMRdmWrmLock);
		return Status;
	}
	mdelay(10);

	up(&ad->NVMRdmWrmLock);
	return Status;
}

static int bcm_char_ioctl_buffer_download(void __user *argp,
					  struct bcm_mini_adapter *ad)
{
	struct bcm_firmware_info *psFwInfo = NULL;
	struct bcm_ioctl_buffer io_buff;
	INT Status;

	BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
		"Starting the firmware download PID =0x%x!!!!\n", current->pid);

	if (!down_trylock(&ad->fw_download_sema)) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"Invalid way to download buffer. Use Start and then call this!!!\n");
		up(&ad->fw_download_sema);
		return -EINVAL;
	}

	/* Copy Ioctl Buffer structure */
	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer))) {
		up(&ad->fw_download_sema);
		return -EFAULT;
	}

	BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
			"Length for FW DLD is : %lx\n", io_buff.InputLength);

	if (io_buff.InputLength > sizeof(struct bcm_firmware_info)) {
		up(&ad->fw_download_sema);
		return -EINVAL;
	}

	psFwInfo = kmalloc(sizeof(*psFwInfo), GFP_KERNEL);
	if (!psFwInfo) {
		up(&ad->fw_download_sema);
		return -ENOMEM;
	}

	if (copy_from_user(psFwInfo, io_buff.InputBuffer,
		io_buff.InputLength)) {
		up(&ad->fw_download_sema);
		kfree(psFwInfo);
		return -EFAULT;
	}

	if (!psFwInfo->pvMappedFirmwareAddress ||
		(psFwInfo->u32FirmwareLength == 0)) {

		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"Something else is wrong %lu\n",
				psFwInfo->u32FirmwareLength);
		up(&ad->fw_download_sema);
		kfree(psFwInfo);
		Status = -EINVAL;
		return Status;
	}

	Status = bcm_ioctl_fw_download(ad, psFwInfo);

	if (Status != STATUS_SUCCESS) {
		if (psFwInfo->u32StartingAddress == CONFIG_BEGIN_ADDR)
			BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"IOCTL: Configuration File Upload Failed\n");
		else
			BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"IOCTL: Firmware File Upload Failed\n");

		/* up(&ad->fw_download_sema); */

		if (ad->LEDInfo.led_thread_running &
			BCM_LED_THREAD_RUNNING_ACTIVELY) {
			ad->DriverState = DRIVER_INIT;
			ad->LEDInfo.bLedInitDone = false;
			wake_up(&ad->LEDInfo.notify_led_event);
		}
	}

	if (Status != STATUS_SUCCESS)
		up(&ad->fw_download_sema);

	BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, OSAL_DBG, DBG_LVL_ALL,
		"IOCTL: Firmware File Uploaded\n");
	kfree(psFwInfo);
	return Status;
}

static int bcm_char_ioctl_buffer_download_stop(void __user *argp,
					       struct bcm_mini_adapter *ad)
{
	INT Status;
	int timeout = 0;

	if (!down_trylock(&ad->fw_download_sema)) {
		up(&ad->fw_download_sema);
		return -EINVAL;
	}

	if (down_trylock(&ad->NVMRdmWrmLock)) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"FW download blocked as EEPROM Read/Write is in progress\n");
		up(&ad->fw_download_sema);
		return -EACCES;
	}

	ad->bBinDownloaded = TRUE;
	ad->bCfgDownloaded = TRUE;
	atomic_set(&ad->CurrNumFreeTxDesc, 0);
	ad->CurrNumRecvDescs = 0;
	ad->downloadDDR = 0;

	/* setting the Mips to Run */
	Status = run_card_proc(ad);

	if (Status) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
			"Firm Download Failed\n");
		up(&ad->fw_download_sema);
		up(&ad->NVMRdmWrmLock);
		return Status;
	} else {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG,
				DBG_LVL_ALL, "Firm Download Over...\n");
	}

	mdelay(10);

	/* Wait for MailBox Interrupt */
	if (StartInterruptUrb((struct bcm_interface_adapter *)ad->pvInterfaceAdapter))
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
			"Unable to send interrupt...\n");

	timeout = 5*HZ;
	ad->waiting_to_fw_download_done = false;
	wait_event_timeout(ad->ioctl_fw_dnld_wait_queue,
			ad->waiting_to_fw_download_done, timeout);
	ad->fw_download_process_pid = INVALID_PID;
	ad->fw_download_done = TRUE;
	atomic_set(&ad->CurrNumFreeTxDesc, 0);
	ad->CurrNumRecvDescs = 0;
	ad->PrevNumRecvDescs = 0;
	atomic_set(&ad->cntrlpktCnt, 0);
	ad->LinkUpStatus = 0;
	ad->LinkStatus = 0;

	if (ad->LEDInfo.led_thread_running &
		BCM_LED_THREAD_RUNNING_ACTIVELY) {
		ad->DriverState = FW_DOWNLOAD_DONE;
		wake_up(&ad->LEDInfo.notify_led_event);
	}

	if (!timeout)
		Status = -ENODEV;

	up(&ad->fw_download_sema);
	up(&ad->NVMRdmWrmLock);
	return Status;
}

static int bcm_char_ioctl_chip_reset(struct bcm_mini_adapter *ad)
{
	INT Status;
	INT NVMAccess;

	NVMAccess = down_trylock(&ad->NVMRdmWrmLock);
	if (NVMAccess) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
			" IOCTL_BCM_CHIP_RESET not allowed as EEPROM Read/Write is in progress\n");
		return -EACCES;
	}

	down(&ad->RxAppControlQueuelock);
	Status = reset_card_proc(ad);
	flushAllAppQ();
	up(&ad->RxAppControlQueuelock);
	up(&ad->NVMRdmWrmLock);
	ResetCounters(ad);
	return Status;
}

static int bcm_char_ioctl_qos_threshold(ULONG arg,
					struct bcm_mini_adapter *ad)
{
	USHORT uiLoopIndex;

	for (uiLoopIndex = 0; uiLoopIndex < NO_OF_QUEUES; uiLoopIndex++) {
		if (get_user(ad->PackInfo[uiLoopIndex].uiThreshold,
				(unsigned long __user *)arg)) {
			return -EFAULT;
		}
	}
	return 0;
}

static int bcm_char_ioctl_switch_transfer_mode(void __user *argp,
					       struct bcm_mini_adapter *ad)
{
	UINT uiData = 0;

	if (copy_from_user(&uiData, argp, sizeof(UINT)))
		return -EFAULT;

	if (uiData) {
		/* Allow All Packets */
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			"IOCTL_BCM_SWITCH_TRANSFER_MODE: ETH_PACKET_TUNNELING_MODE\n");
			ad->TransferMode = ETH_PACKET_TUNNELING_MODE;
	} else {
		/* Allow IP only Packets */
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			"IOCTL_BCM_SWITCH_TRANSFER_MODE: IP_PACKET_ONLY_MODE\n");
		ad->TransferMode = IP_PACKET_ONLY_MODE;
	}
	return STATUS_SUCCESS;
}

static int bcm_char_ioctl_get_driver_version(void __user *argp)
{
	struct bcm_ioctl_buffer io_buff;
	ulong len;

	/* Copy Ioctl Buffer structure */
	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer)))
		return -EFAULT;

	len = min_t(ulong, io_buff.OutputLength, strlen(DRV_VERSION) + 1);

	if (copy_to_user(io_buff.OutputBuffer, DRV_VERSION, len))
		return -EFAULT;

	return STATUS_SUCCESS;
}

static int bcm_char_ioctl_get_current_status(void __user *argp,
					     struct bcm_mini_adapter *ad)
{
	struct bcm_link_state link_state;
	struct bcm_ioctl_buffer io_buff;

	/* Copy Ioctl Buffer structure */
	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer))) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
			"copy_from_user failed..\n");
		return -EFAULT;
	}

	if (io_buff.OutputLength != sizeof(link_state))
		return -EINVAL;

	memset(&link_state, 0, sizeof(link_state));
	link_state.bIdleMode = ad->IdleMode;
	link_state.bShutdownMode = ad->bShutStatus;
	link_state.ucLinkStatus = ad->LinkStatus;

	if (copy_to_user(io_buff.OutputBuffer, &link_state, min_t(size_t,
		sizeof(link_state), io_buff.OutputLength))) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
			"Copy_to_user Failed..\n");
		return -EFAULT;
	}
	return STATUS_SUCCESS;
}


static int bcm_char_ioctl_set_mac_tracing(void __user *argp,
					  struct bcm_mini_adapter *ad)
{
	struct bcm_ioctl_buffer io_buff;
	UINT tracing_flag;

	/* copy ioctl Buffer structure */
	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer)))
		return -EFAULT;

	if (copy_from_user(&tracing_flag, io_buff.InputBuffer, sizeof(UINT)))
		return -EFAULT;

	if (tracing_flag)
		ad->pTarangs->MacTracingEnabled = TRUE;
	else
		ad->pTarangs->MacTracingEnabled = false;

	return STATUS_SUCCESS;
}

static int bcm_char_ioctl_get_dsx_indication(void __user *argp,
					     struct bcm_mini_adapter *ad)
{
	struct bcm_ioctl_buffer io_buff;
	ULONG ulSFId = 0;

	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer)))
		return -EFAULT;

	if (io_buff.OutputLength < sizeof(struct bcm_add_indication_alt)) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
			"Mismatch req: %lx needed is =0x%zx!!!",
			io_buff.OutputLength,
			sizeof(struct bcm_add_indication_alt));
		return -EINVAL;
	}

	if (copy_from_user(&ulSFId, io_buff.InputBuffer, sizeof(ulSFId)))
		return -EFAULT;

	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
		"Get DSX Data SF ID is =%lx\n", ulSFId);
	get_dsx_sf_data_to_application(ad, ulSFId, io_buff.OutputBuffer);
	return STATUS_SUCCESS;
}

static int bcm_char_ioctl_get_host_mibs(void __user *argp,
					struct bcm_mini_adapter *ad,
					struct bcm_tarang_data *pTarang)
{
	struct bcm_ioctl_buffer io_buff;
	INT Status = STATUS_FAILURE;
	PVOID temp_buff;

	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer)))
		return -EFAULT;

	if (io_buff.OutputLength != sizeof(struct bcm_host_stats_mibs)) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
			"Length Check failed %lu %zd\n", io_buff.OutputLength,
			sizeof(struct bcm_host_stats_mibs));
		return -EINVAL;
	}

	/* FIXME: HOST_STATS are too big for kmalloc (122048)! */
	temp_buff = kzalloc(sizeof(struct bcm_host_stats_mibs), GFP_KERNEL);
	if (!temp_buff)
		return STATUS_FAILURE;

	Status = ProcessGetHostMibs(ad, temp_buff);
	GetDroppedAppCntrlPktMibs(temp_buff, pTarang);

	if (Status != STATUS_FAILURE) {
		if (copy_to_user(io_buff.OutputBuffer, temp_buff,
			sizeof(struct bcm_host_stats_mibs))) {
			kfree(temp_buff);
			return -EFAULT;
		}
	}

	kfree(temp_buff);
	return Status;
}

static int bcm_char_ioctl_bulk_wrm(void __user *argp,
				   struct bcm_mini_adapter *ad, UINT cmd)
{
	struct bcm_bulk_wrm_buffer *pBulkBuffer;
	struct bcm_ioctl_buffer io_buff;
	UINT uiTempVar = 0;
	INT Status = STATUS_FAILURE;
	PCHAR pvBuffer = NULL;

	if ((ad->IdleMode == TRUE) ||
		(ad->bShutStatus == TRUE) ||
		(ad->bPreparingForLowPowerMode == TRUE)) {

		BCM_DEBUG_PRINT (ad, DBG_TYPE_PRINTK, 0, 0,
			"Device in Idle/Shutdown Mode, Blocking Wrms\n");
		return -EACCES;
	}

	/* Copy Ioctl Buffer structure */
	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer)))
		return -EFAULT;

	if (io_buff.InputLength < sizeof(ULONG) * 2)
		return -EINVAL;

	pvBuffer = memdup_user(io_buff.InputBuffer,
			       io_buff.InputLength);
	if (IS_ERR(pvBuffer))
		return PTR_ERR(pvBuffer);

	pBulkBuffer = (struct bcm_bulk_wrm_buffer *)pvBuffer;

	if (((ULONG)pBulkBuffer->Register & 0x0F000000) != 0x0F000000 ||
		((ULONG)pBulkBuffer->Register & 0x3)) {
		BCM_DEBUG_PRINT (ad, DBG_TYPE_PRINTK, 0, 0,
			"WRM Done On invalid Address : %x Access Denied.\n",
			(int)pBulkBuffer->Register);
		kfree(pvBuffer);
		return -EINVAL;
	}

	uiTempVar = pBulkBuffer->Register & EEPROM_REJECT_MASK;
	if (!((ad->pstargetparams->m_u32Customize)&VSG_MODE) &&
		((uiTempVar == EEPROM_REJECT_REG_1) ||
			(uiTempVar == EEPROM_REJECT_REG_2) ||
			(uiTempVar == EEPROM_REJECT_REG_3) ||
			(uiTempVar == EEPROM_REJECT_REG_4)) &&
		(cmd == IOCTL_BCM_REGISTER_WRITE)) {

		kfree(pvBuffer);
		BCM_DEBUG_PRINT (ad, DBG_TYPE_PRINTK, 0, 0,
			"EEPROM Access Denied, not in VSG Mode\n");
		return -EFAULT;
	}

	if (pBulkBuffer->SwapEndian == false)
		Status = wrmWithLock(ad, (UINT)pBulkBuffer->Register,
			(PCHAR)pBulkBuffer->Values,
			io_buff.InputLength - 2*sizeof(ULONG));
	else
		Status = wrmaltWithLock(ad, (UINT)pBulkBuffer->Register,
			(PUINT)pBulkBuffer->Values,
			io_buff.InputLength - 2*sizeof(ULONG));

	if (Status != STATUS_SUCCESS)
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0, "WRM Failed\n");

	kfree(pvBuffer);
	return Status;
}

static int bcm_char_ioctl_get_nvm_size(void __user *argp,
				       struct bcm_mini_adapter *ad)
{
	struct bcm_ioctl_buffer io_buff;

	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer)))
		return -EFAULT;

	if (ad->eNVMType == NVM_EEPROM || ad->eNVMType == NVM_FLASH) {
		if (copy_to_user(io_buff.OutputBuffer, &ad->uiNVMDSDSize,
			sizeof(UINT)))
			return -EFAULT;
	}

	return STATUS_SUCCESS;
}

static int bcm_char_ioctl_cal_init(void __user *argp,
				   struct bcm_mini_adapter *ad)
{
	struct bcm_ioctl_buffer io_buff;
	UINT uiSectorSize = 0;
	INT Status = STATUS_FAILURE;

	if (ad->eNVMType == NVM_FLASH) {
		if (copy_from_user(&io_buff, argp,
			sizeof(struct bcm_ioctl_buffer)))
			return -EFAULT;

		if (copy_from_user(&uiSectorSize, io_buff.InputBuffer,
			sizeof(UINT)))
			return -EFAULT;

		if ((uiSectorSize < MIN_SECTOR_SIZE) ||
			(uiSectorSize > MAX_SECTOR_SIZE)) {
			if (copy_to_user(io_buff.OutputBuffer,
				&ad->uiSectorSize, sizeof(UINT)))
				return -EFAULT;
		} else {
			if (IsFlash2x(ad)) {
				if (copy_to_user(io_buff.OutputBuffer,
					&ad->uiSectorSize, sizeof(UINT)))
					return -EFAULT;
			} else {
				if ((TRUE == ad->bShutStatus) ||
					(TRUE == ad->IdleMode)) {
					BCM_DEBUG_PRINT(ad,
						DBG_TYPE_PRINTK, 0, 0,
						"Device is in Idle/Shutdown Mode\n");
					return -EACCES;
				}

				ad->uiSectorSize = uiSectorSize;
				BcmUpdateSectorSize(ad,
					ad->uiSectorSize);
			}
		}
		Status = STATUS_SUCCESS;
	} else {
		Status = STATUS_FAILURE;
	}
	return Status;
}

static int bcm_char_ioctl_set_debug(void __user *argp,
				    struct bcm_mini_adapter *ad)
{
#ifdef DEBUG
	struct bcm_ioctl_buffer io_buff;
	struct bcm_user_debug_state sUserDebugState;

	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
		"In SET_DEBUG ioctl\n");
	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer)))
		return -EFAULT;

	if (copy_from_user(&sUserDebugState, io_buff.InputBuffer,
		sizeof(struct bcm_user_debug_state)))
		return -EFAULT;

	BCM_DEBUG_PRINT (ad, DBG_TYPE_PRINTK, 0, 0,
			"IOCTL_BCM_SET_DEBUG: OnOff=%d Type = 0x%x ",
			sUserDebugState.OnOff, sUserDebugState.Type);
	/* sUserDebugState.Subtype <<= 1; */
	sUserDebugState.Subtype = 1 << sUserDebugState.Subtype;
	BCM_DEBUG_PRINT (ad, DBG_TYPE_PRINTK, 0, 0,
		"actual Subtype=0x%x\n", sUserDebugState.Subtype);

	/* Update new 'DebugState' in the ad */
	ad->stDebugState.type |= sUserDebugState.Type;
	/* Subtype: A bitmap of 32 bits for Subtype per Type.
	 * Valid indexes in 'subtype' array: 1,2,4,8
	 * corresponding to valid Type values. Hence we can use the 'Type' field
	 * as the index value, ignoring the array entries 0,3,5,6,7 !
	 */
	if (sUserDebugState.OnOff)
		ad->stDebugState.subtype[sUserDebugState.Type] |=
			sUserDebugState.Subtype;
	else
		ad->stDebugState.subtype[sUserDebugState.Type] &=
			~sUserDebugState.Subtype;

	BCM_SHOW_DEBUG_BITMAP(ad);
#endif
	return STATUS_SUCCESS;
}

static int bcm_char_ioctl_nvm_rw(void __user *argp,
				 struct bcm_mini_adapter *ad, UINT cmd)
{
	struct bcm_nvm_readwrite stNVMReadWrite;
	struct timeval tv0, tv1;
	struct bcm_ioctl_buffer io_buff;
	PUCHAR pReadData = NULL;
	INT Status = STATUS_FAILURE;

	memset(&tv0, 0, sizeof(struct timeval));
	memset(&tv1, 0, sizeof(struct timeval));
	if ((ad->eNVMType == NVM_FLASH) &&
		(ad->uiFlashLayoutMajorVersion == 0)) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
			"The Flash Control Section is Corrupted. Hence Rejection on NVM Read/Write\n");
		return -EFAULT;
	}

	if (IsFlash2x(ad)) {
		if ((ad->eActiveDSD != DSD0) &&
			(ad->eActiveDSD != DSD1) &&
			(ad->eActiveDSD != DSD2)) {

			BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"No DSD is active..hence NVM Command is blocked");
			return STATUS_FAILURE;
		}
	}

	/* Copy Ioctl Buffer structure */
	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer)))
		return -EFAULT;

	if (copy_from_user(&stNVMReadWrite,
				(IOCTL_BCM_NVM_READ == cmd) ?
				io_buff.OutputBuffer : io_buff.InputBuffer,
				sizeof(struct bcm_nvm_readwrite)))
		return -EFAULT;

	/*
	 * Deny the access if the offset crosses the cal area limit.
	 */
	if (stNVMReadWrite.uiNumBytes > ad->uiNVMDSDSize)
		return STATUS_FAILURE;

	if (stNVMReadWrite.uiOffset >
		ad->uiNVMDSDSize - stNVMReadWrite.uiNumBytes)
		return STATUS_FAILURE;

	pReadData = memdup_user(stNVMReadWrite.pBuffer,
				stNVMReadWrite.uiNumBytes);
	if (IS_ERR(pReadData))
		return PTR_ERR(pReadData);

	do_gettimeofday(&tv0);
	if (IOCTL_BCM_NVM_READ == cmd) {
		int ret = bcm_handle_nvm_read_cmd(ad, pReadData,
				&stNVMReadWrite);
		if (ret != STATUS_SUCCESS)
			return ret;
	} else {
		down(&ad->NVMRdmWrmLock);

		if ((ad->IdleMode == TRUE) ||
			(ad->bShutStatus == TRUE) ||
			(ad->bPreparingForLowPowerMode == TRUE)) {

			BCM_DEBUG_PRINT(ad,
				DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
				"Device is in Idle/Shutdown Mode\n");
			up(&ad->NVMRdmWrmLock);
			kfree(pReadData);
			return -EACCES;
		}

		ad->bHeaderChangeAllowed = TRUE;
		if (IsFlash2x(ad)) {
			int ret = handle_flash2x_adapter(ad,
							pReadData,
							&stNVMReadWrite);
			if (ret != STATUS_SUCCESS)
				return ret;
		}

		Status = BeceemNVMWrite(ad, (PUINT)pReadData,
			stNVMReadWrite.uiOffset, stNVMReadWrite.uiNumBytes,
			stNVMReadWrite.bVerify);
		if (IsFlash2x(ad))
			BcmFlash2xWriteSig(ad, ad->eActiveDSD);

		ad->bHeaderChangeAllowed = false;

		up(&ad->NVMRdmWrmLock);

		if (Status != STATUS_SUCCESS) {
			kfree(pReadData);
			return Status;
		}
	}

	do_gettimeofday(&tv1);
	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
		" timetaken by Write/read :%ld msec\n",
		(tv1.tv_sec - tv0.tv_sec)*1000 +
		(tv1.tv_usec - tv0.tv_usec)/1000);

	kfree(pReadData);
	return STATUS_SUCCESS;
}

static int bcm_char_ioctl_flash2x_section_read(void __user *argp,
	struct bcm_mini_adapter *ad)
{
	struct bcm_flash2x_readwrite sFlash2xRead = {0};
	struct bcm_ioctl_buffer io_buff;
	PUCHAR pReadBuff = NULL;
	UINT NOB = 0;
	UINT BuffSize = 0;
	UINT ReadBytes = 0;
	UINT ReadOffset = 0;
	INT Status = STATUS_FAILURE;
	void __user *OutPutBuff;

	if (IsFlash2x(ad) != TRUE)	{
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
			"Flash Does not have 2.x map");
		return -EINVAL;
	}

	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG,
		DBG_LVL_ALL, "IOCTL_BCM_FLASH2X_SECTION_READ Called");
	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer)))
		return -EFAULT;

	/* Reading FLASH 2.x READ structure */
	if (copy_from_user(&sFlash2xRead, io_buff.InputBuffer,
		sizeof(struct bcm_flash2x_readwrite)))
		return -EFAULT;

	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			"\nsFlash2xRead.Section :%x",
			sFlash2xRead.Section);
	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			"\nsFlash2xRead.offset :%x",
			sFlash2xRead.offset);
	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			"\nsFlash2xRead.numOfBytes :%x",
			sFlash2xRead.numOfBytes);
	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			"\nsFlash2xRead.bVerify :%x\n",
			sFlash2xRead.bVerify);

	/* This was internal to driver for raw read.
	 * now it has ben exposed to user space app.
	 */
	if (validateFlash2xReadWrite(ad, &sFlash2xRead) == false)
		return STATUS_FAILURE;

	NOB = sFlash2xRead.numOfBytes;
	if (NOB > ad->uiSectorSize)
		BuffSize = ad->uiSectorSize;
	else
		BuffSize = NOB;

	ReadOffset = sFlash2xRead.offset;
	OutPutBuff = io_buff.OutputBuffer;
	pReadBuff = kzalloc(BuffSize , GFP_KERNEL);

	if (pReadBuff == NULL) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"Memory allocation failed for Flash 2.x Read Structure");
		return -ENOMEM;
	}
	down(&ad->NVMRdmWrmLock);

	if ((ad->IdleMode == TRUE) ||
		(ad->bShutStatus == TRUE) ||
		(ad->bPreparingForLowPowerMode == TRUE)) {

		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG,
				DBG_LVL_ALL,
				"Device is in Idle/Shutdown Mode\n");
		up(&ad->NVMRdmWrmLock);
		kfree(pReadBuff);
		return -EACCES;
	}

	while (NOB) {
		if (NOB > ad->uiSectorSize)
			ReadBytes = ad->uiSectorSize;
		else
			ReadBytes = NOB;

		/* Reading the data from Flash 2.x */
		Status = BcmFlash2xBulkRead(ad, (PUINT)pReadBuff,
			sFlash2xRead.Section, ReadOffset, ReadBytes);
		if (Status) {
			BCM_DEBUG_PRINT(ad,
				DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
				"Flash 2x read err with Status :%d",
				Status);
			break;
		}

		BCM_DEBUG_PRINT_BUFFER(ad, DBG_TYPE_OTHERS, OSAL_DBG,
			DBG_LVL_ALL, pReadBuff, ReadBytes);

		Status = copy_to_user(OutPutBuff, pReadBuff, ReadBytes);
		if (Status) {
			BCM_DEBUG_PRINT(ad,
				DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
				"Copy to use failed with status :%d", Status);
			up(&ad->NVMRdmWrmLock);
			kfree(pReadBuff);
			return -EFAULT;
		}
		NOB = NOB - ReadBytes;
		if (NOB) {
			ReadOffset = ReadOffset + ReadBytes;
			OutPutBuff = OutPutBuff + ReadBytes;
		}
	}

	up(&ad->NVMRdmWrmLock);
	kfree(pReadBuff);
	return Status;
}

static int bcm_char_ioctl_flash2x_section_write(void __user *argp,
	struct bcm_mini_adapter *ad)
{
	struct bcm_flash2x_readwrite sFlash2xWrite = {0};
	struct bcm_ioctl_buffer io_buff;
	PUCHAR pWriteBuff;
	void __user *InputAddr;
	UINT NOB = 0;
	UINT BuffSize = 0;
	UINT WriteOffset = 0;
	UINT WriteBytes = 0;
	INT Status = STATUS_FAILURE;

	if (IsFlash2x(ad) != TRUE) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
			"Flash Does not have 2.x map");
		return -EINVAL;
	}

	/* First make this False so that we can enable the Sector
	 * Permission Check in BeceemFlashBulkWrite
	 */
	ad->bAllDSDWriteAllow = false;

	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
		"IOCTL_BCM_FLASH2X_SECTION_WRITE Called");

	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer)))
		return -EFAULT;

	/* Reading FLASH 2.x READ structure */
	if (copy_from_user(&sFlash2xWrite, io_buff.InputBuffer,
		sizeof(struct bcm_flash2x_readwrite)))
		return -EFAULT;

	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
		"\nsFlash2xWrite.Section :%x", sFlash2xWrite.Section);
	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
		"\nsFlash2xWrite.offset :%d", sFlash2xWrite.offset);
	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
		"\nsFlash2xWrite.numOfBytes :%x", sFlash2xWrite.numOfBytes);
	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
		"\nsFlash2xWrite.bVerify :%x\n", sFlash2xWrite.bVerify);

	if ((sFlash2xWrite.Section != VSA0) && (sFlash2xWrite.Section != VSA1)
		&& (sFlash2xWrite.Section != VSA2)) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			"Only VSA write is allowed");
		return -EINVAL;
	}

	if (validateFlash2xReadWrite(ad, &sFlash2xWrite) == false)
		return STATUS_FAILURE;

	InputAddr = sFlash2xWrite.pDataBuff;
	WriteOffset = sFlash2xWrite.offset;
	NOB = sFlash2xWrite.numOfBytes;

	if (NOB > ad->uiSectorSize)
		BuffSize = ad->uiSectorSize;
	else
		BuffSize = NOB;

	pWriteBuff = kmalloc(BuffSize, GFP_KERNEL);

	if (pWriteBuff == NULL)
		return -ENOMEM;

	/* extracting the remainder of the given offset. */
	WriteBytes = ad->uiSectorSize;
	if (WriteOffset % ad->uiSectorSize) {
		WriteBytes = ad->uiSectorSize -
			(WriteOffset % ad->uiSectorSize);
	}

	if (NOB < WriteBytes)
		WriteBytes = NOB;

	down(&ad->NVMRdmWrmLock);

	if ((ad->IdleMode == TRUE) ||
		(ad->bShutStatus == TRUE) ||
		(ad->bPreparingForLowPowerMode == TRUE)) {

		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			"Device is in Idle/Shutdown Mode\n");
		up(&ad->NVMRdmWrmLock);
		kfree(pWriteBuff);
		return -EACCES;
	}

	BcmFlash2xCorruptSig(ad, sFlash2xWrite.Section);
	do {
		Status = copy_from_user(pWriteBuff, InputAddr, WriteBytes);
		if (Status) {
			BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"Copy to user failed with status :%d", Status);
			up(&ad->NVMRdmWrmLock);
			kfree(pWriteBuff);
			return -EFAULT;
		}
		BCM_DEBUG_PRINT_BUFFER(ad, DBG_TYPE_OTHERS,
			OSAL_DBG, DBG_LVL_ALL, pWriteBuff, WriteBytes);

		/* Writing the data from Flash 2.x */
		Status = BcmFlash2xBulkWrite(ad, (PUINT)pWriteBuff,
					     sFlash2xWrite.Section,
					     WriteOffset,
					     WriteBytes,
					     sFlash2xWrite.bVerify);

		if (Status) {
			BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"Flash 2x read err with Status :%d", Status);
			break;
		}

		NOB = NOB - WriteBytes;
		if (NOB) {
			WriteOffset = WriteOffset + WriteBytes;
			InputAddr = InputAddr + WriteBytes;
			if (NOB > ad->uiSectorSize)
				WriteBytes = ad->uiSectorSize;
			else
				WriteBytes = NOB;
		}
	} while (NOB > 0);

	BcmFlash2xWriteSig(ad, sFlash2xWrite.Section);
	up(&ad->NVMRdmWrmLock);
	kfree(pWriteBuff);
	return Status;
}

static int bcm_char_ioctl_flash2x_section_bitmap(void __user *argp,
	struct bcm_mini_adapter *ad)
{
	struct bcm_flash2x_bitmap *psFlash2xBitMap;
	struct bcm_ioctl_buffer io_buff;

BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
	"IOCTL_BCM_GET_FLASH2X_SECTION_BITMAP Called");

	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer)))
		return -EFAULT;

	if (io_buff.OutputLength != sizeof(struct bcm_flash2x_bitmap))
		return -EINVAL;

	psFlash2xBitMap = kzalloc(sizeof(struct bcm_flash2x_bitmap),
			GFP_KERNEL);

	if (psFlash2xBitMap == NULL) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
			"Memory is not available");
		return -ENOMEM;
	}

	/* Reading the Flash Sectio Bit map */
	down(&ad->NVMRdmWrmLock);

	if ((ad->IdleMode == TRUE) ||
		(ad->bShutStatus == TRUE) ||
		(ad->bPreparingForLowPowerMode == TRUE)) {

		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			"Device is in Idle/Shutdown Mode\n");
		up(&ad->NVMRdmWrmLock);
		kfree(psFlash2xBitMap);
		return -EACCES;
	}

	BcmGetFlash2xSectionalBitMap(ad, psFlash2xBitMap);
	up(&ad->NVMRdmWrmLock);
	if (copy_to_user(io_buff.OutputBuffer, psFlash2xBitMap,
		sizeof(struct bcm_flash2x_bitmap))) {
		kfree(psFlash2xBitMap);
		return -EFAULT;
	}

	kfree(psFlash2xBitMap);
	return STATUS_FAILURE;
}

static int bcm_char_ioctl_set_active_section(void __user *argp,
					     struct bcm_mini_adapter *ad)
{
	enum bcm_flash2x_section_val eFlash2xSectionVal = 0;
	INT Status = STATUS_FAILURE;
	struct bcm_ioctl_buffer io_buff;

	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			"IOCTL_BCM_SET_ACTIVE_SECTION Called");

	if (IsFlash2x(ad) != TRUE) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"Flash Does not have 2.x map");
		return -EINVAL;
	}

	Status = copy_from_user(&io_buff, argp,
				sizeof(struct bcm_ioctl_buffer));
	if (Status) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"Copy of IOCTL BUFFER failed");
		return -EFAULT;
	}

	Status = copy_from_user(&eFlash2xSectionVal,
				io_buff.InputBuffer, sizeof(INT));
	if (Status) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
			"Copy of flash section val failed");
		return -EFAULT;
	}

	down(&ad->NVMRdmWrmLock);

	if ((ad->IdleMode == TRUE) ||
		(ad->bShutStatus == TRUE) ||
		(ad->bPreparingForLowPowerMode == TRUE)) {

		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
				"Device is in Idle/Shutdown Mode\n");
		up(&ad->NVMRdmWrmLock);
		return -EACCES;
	}

	Status = BcmSetActiveSection(ad, eFlash2xSectionVal);
	if (Status)
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"Failed to make it's priority Highest. Status %d",
				Status);

	up(&ad->NVMRdmWrmLock);

	return Status;
}

static int bcm_char_ioctl_copy_section(void __user *argp,
				       struct bcm_mini_adapter *ad)
{
	struct bcm_flash2x_copy_section sCopySectStrut = {0};
	struct bcm_ioctl_buffer io_buff;
	INT Status = STATUS_SUCCESS;

	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			"IOCTL_BCM_COPY_SECTION  Called");

	ad->bAllDSDWriteAllow = false;
	if (IsFlash2x(ad) != TRUE) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"Flash Does not have 2.x map");
		return -EINVAL;
	}

	Status = copy_from_user(&io_buff, argp,
				sizeof(struct bcm_ioctl_buffer));
	if (Status) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"Copy of IOCTL BUFFER failed Status :%d",
				Status);
		return -EFAULT;
	}

	Status = copy_from_user(&sCopySectStrut, io_buff.InputBuffer,
				sizeof(struct bcm_flash2x_copy_section));
	if (Status) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"Copy of Copy_Section_Struct failed with Status :%d",
				Status);
		return -EFAULT;
	}

	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			"Source SEction :%x", sCopySectStrut.SrcSection);
	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			"Destination SEction :%x", sCopySectStrut.DstSection);
	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			"offset :%x", sCopySectStrut.offset);
	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			"NOB :%x", sCopySectStrut.numOfBytes);

	if (IsSectionExistInFlash(ad, sCopySectStrut.SrcSection) == false) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"Source Section<%x> does not exist in Flash ",
				sCopySectStrut.SrcSection);
		return -EINVAL;
	}

	if (IsSectionExistInFlash(ad, sCopySectStrut.DstSection) == false) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"Destinatio Section<%x> does not exist in Flash ",
				sCopySectStrut.DstSection);
		return -EINVAL;
	}

	if (sCopySectStrut.SrcSection == sCopySectStrut.DstSection) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
				"Source and Destination section should be different");
		return -EINVAL;
	}

	down(&ad->NVMRdmWrmLock);

	if ((ad->IdleMode == TRUE) ||
		(ad->bShutStatus == TRUE) ||
		(ad->bPreparingForLowPowerMode == TRUE)) {

		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
				"Device is in Idle/Shutdown Mode\n");
		up(&ad->NVMRdmWrmLock);
		return -EACCES;
	}

	if (sCopySectStrut.SrcSection == ISO_IMAGE1 ||
		sCopySectStrut.SrcSection == ISO_IMAGE2) {
		if (IsNonCDLessDevice(ad)) {
			BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
					"Device is Non-CDLess hence won't have ISO !!");
			Status = -EINVAL;
		} else if (sCopySectStrut.numOfBytes == 0) {
			Status = BcmCopyISO(ad, sCopySectStrut);
		} else {
			BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
					"Partial Copy of ISO section is not Allowed..");
			Status = STATUS_FAILURE;
		}
		up(&ad->NVMRdmWrmLock);
		return Status;
	}

	Status = BcmCopySection(ad, sCopySectStrut.SrcSection,
				sCopySectStrut.DstSection,
				sCopySectStrut.offset,
				sCopySectStrut.numOfBytes);
	up(&ad->NVMRdmWrmLock);
	return Status;
}

static int bcm_char_ioctl_get_flash_cs_info(void __user *argp,
					    struct bcm_mini_adapter *ad)
{
	struct bcm_ioctl_buffer io_buff;
	INT Status = STATUS_SUCCESS;

	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			" IOCTL_BCM_GET_FLASH_CS_INFO Called");

	Status = copy_from_user(&io_buff, argp,
			sizeof(struct bcm_ioctl_buffer));
	if (Status) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"Copy of IOCTL BUFFER failed");
		return -EFAULT;
	}

	if (ad->eNVMType != NVM_FLASH) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"Connected device does not have flash");
		return -EINVAL;
	}

	if (IsFlash2x(ad) == TRUE) {
		if (io_buff.OutputLength < sizeof(struct bcm_flash2x_cs_info))
			return -EINVAL;

		if (copy_to_user(io_buff.OutputBuffer,
				 ad->psFlash2xCSInfo,
				 sizeof(struct bcm_flash2x_cs_info)))
			return -EFAULT;
	} else {
		if (io_buff.OutputLength < sizeof(struct bcm_flash_cs_info))
			return -EINVAL;

		if (copy_to_user(io_buff.OutputBuffer, ad->psFlashCSInfo,
				 sizeof(struct bcm_flash_cs_info)))
			return -EFAULT;
	}
	return Status;
}

static int bcm_char_ioctl_select_dsd(void __user *argp,
				     struct bcm_mini_adapter *ad)
{
	struct bcm_ioctl_buffer io_buff;
	INT Status = STATUS_FAILURE;
	UINT SectOfset = 0;
	enum bcm_flash2x_section_val eFlash2xSectionVal;

	eFlash2xSectionVal = NO_SECTION_VAL;
	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			"IOCTL_BCM_SELECT_DSD Called");

	if (IsFlash2x(ad) != TRUE) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"Flash Does not have 2.x map");
		return -EINVAL;
	}

	Status = copy_from_user(&io_buff, argp,
				sizeof(struct bcm_ioctl_buffer));
	if (Status) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"Copy of IOCTL BUFFER failed");
		return -EFAULT;
	}
	Status = copy_from_user(&eFlash2xSectionVal, io_buff.InputBuffer,
		sizeof(INT));
	if (Status) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"Copy of flash section val failed");
		return -EFAULT;
	}

	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			"Read Section :%d", eFlash2xSectionVal);
	if ((eFlash2xSectionVal != DSD0) &&
		(eFlash2xSectionVal != DSD1) &&
		(eFlash2xSectionVal != DSD2)) {

		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"Passed section<%x> is not DSD section",
				eFlash2xSectionVal);
		return STATUS_FAILURE;
	}

	SectOfset = BcmGetSectionValStartOffset(ad, eFlash2xSectionVal);
	if (SectOfset == INVALID_OFFSET) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"Provided Section val <%d> does not exist in Flash 2.x",
				eFlash2xSectionVal);
		return -EINVAL;
	}

	ad->bAllDSDWriteAllow = TRUE;
	ad->ulFlashCalStart = SectOfset;
	ad->eActiveDSD = eFlash2xSectionVal;

	return STATUS_SUCCESS;
}

static int bcm_char_ioctl_nvm_raw_read(void __user *argp,
				       struct bcm_mini_adapter *ad)
{
	struct bcm_nvm_readwrite stNVMRead;
	struct bcm_ioctl_buffer io_buff;
	unsigned int NOB;
	INT BuffSize;
	INT ReadOffset = 0;
	UINT ReadBytes = 0;
	PUCHAR pReadBuff;
	void __user *OutPutBuff;
	INT Status = STATUS_FAILURE;

	if (ad->eNVMType != NVM_FLASH) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"NVM TYPE is not Flash");
		return -EINVAL;
	}

	/* Copy Ioctl Buffer structure */
	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer))) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"copy_from_user 1 failed\n");
		return -EFAULT;
	}

	if (copy_from_user(&stNVMRead, io_buff.OutputBuffer,
		sizeof(struct bcm_nvm_readwrite)))
		return -EFAULT;

	NOB = stNVMRead.uiNumBytes;
	/* In Raw-Read max Buff size : 64MB */

	if (NOB > DEFAULT_BUFF_SIZE)
		BuffSize = DEFAULT_BUFF_SIZE;
	else
		BuffSize = NOB;

	ReadOffset = stNVMRead.uiOffset;
	OutPutBuff = stNVMRead.pBuffer;

	pReadBuff = kzalloc(BuffSize , GFP_KERNEL);
	if (pReadBuff == NULL) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
				"Memory allocation failed for Flash 2.x Read Structure");
		return -ENOMEM;
	}
	down(&ad->NVMRdmWrmLock);

	if ((ad->IdleMode == TRUE) ||
		(ad->bShutStatus == TRUE) ||
		(ad->bPreparingForLowPowerMode == TRUE)) {

		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
				"Device is in Idle/Shutdown Mode\n");
		kfree(pReadBuff);
		up(&ad->NVMRdmWrmLock);
		return -EACCES;
	}

	ad->bFlashRawRead = TRUE;

	while (NOB) {
		if (NOB > DEFAULT_BUFF_SIZE)
			ReadBytes = DEFAULT_BUFF_SIZE;
		else
			ReadBytes = NOB;

		/* Reading the data from Flash 2.x */
		Status = BeceemNVMRead(ad, (PUINT)pReadBuff,
			ReadOffset, ReadBytes);
		if (Status) {
			BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
					"Flash 2x read err with Status :%d",
					Status);
			break;
		}

		BCM_DEBUG_PRINT_BUFFER(ad, DBG_TYPE_OTHERS, OSAL_DBG,
				       DBG_LVL_ALL, pReadBuff, ReadBytes);

		Status = copy_to_user(OutPutBuff, pReadBuff, ReadBytes);
		if (Status) {
			BCM_DEBUG_PRINT(ad, DBG_TYPE_PRINTK, 0, 0,
					"Copy to use failed with status :%d",
					Status);
			up(&ad->NVMRdmWrmLock);
			kfree(pReadBuff);
			return -EFAULT;
		}
		NOB = NOB - ReadBytes;
		if (NOB) {
			ReadOffset = ReadOffset + ReadBytes;
			OutPutBuff = OutPutBuff + ReadBytes;
		}
	}
	ad->bFlashRawRead = false;
	up(&ad->NVMRdmWrmLock);
	kfree(pReadBuff);
	return Status;
}

static int bcm_char_ioctl_cntrlmsg_mask(void __user *argp,
					struct bcm_mini_adapter *ad,
					struct bcm_tarang_data *pTarang)
{
	struct bcm_ioctl_buffer io_buff;
	INT Status = STATUS_FAILURE;
	ULONG RxCntrlMsgBitMask = 0;

	/* Copy Ioctl Buffer structure */
	Status = copy_from_user(&io_buff, argp,
			sizeof(struct bcm_ioctl_buffer));
	if (Status) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
				"copy of Ioctl buffer is failed from user space");
		return -EFAULT;
	}

	if (io_buff.InputLength != sizeof(unsigned long))
		return -EINVAL;

	Status = copy_from_user(&RxCntrlMsgBitMask, io_buff.InputBuffer,
				io_buff.InputLength);
	if (Status) {
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
				"copy of control bit mask failed from user space");
		return -EFAULT;
	}
	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			"\n Got user defined cntrl msg bit mask :%lx",
			RxCntrlMsgBitMask);
	pTarang->RxCntrlMsgBitMask = RxCntrlMsgBitMask;

	return Status;
}

static int bcm_char_ioctl_get_device_driver_info(void __user *argp,
	struct bcm_mini_adapter *ad)
{
	struct bcm_driver_info DevInfo;
	struct bcm_ioctl_buffer io_buff;

	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			"Called IOCTL_BCM_GET_DEVICE_DRIVER_INFO\n");

	memset(&DevInfo, 0, sizeof(DevInfo));
	DevInfo.MaxRDMBufferSize = BUFFER_4K;
	DevInfo.u32DSDStartOffset = EEPROM_CALPARAM_START;
	DevInfo.u32RxAlignmentCorrection = 0;
	DevInfo.u32NVMType = ad->eNVMType;
	DevInfo.u32InterfaceType = BCM_USB;

	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer)))
		return -EFAULT;

	if (io_buff.OutputLength < sizeof(DevInfo))
		return -EINVAL;

	if (copy_to_user(io_buff.OutputBuffer, &DevInfo, sizeof(DevInfo)))
		return -EFAULT;

	return STATUS_SUCCESS;
}

static int bcm_char_ioctl_time_since_net_entry(void __user *argp,
	struct bcm_mini_adapter *ad)
{
	struct bcm_time_elapsed stTimeElapsedSinceNetEntry = {0};
	struct bcm_ioctl_buffer io_buff;

	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			"IOCTL_BCM_TIME_SINCE_NET_ENTRY called");

	if (copy_from_user(&io_buff, argp, sizeof(struct bcm_ioctl_buffer)))
		return -EFAULT;

	if (io_buff.OutputLength < sizeof(struct bcm_time_elapsed))
		return -EINVAL;

	stTimeElapsedSinceNetEntry.ul64TimeElapsedSinceNetEntry =
		get_seconds() - ad->liTimeSinceLastNetEntry;

	if (copy_to_user(io_buff.OutputBuffer, &stTimeElapsedSinceNetEntry,
			 sizeof(struct bcm_time_elapsed)))
		return -EFAULT;

	return STATUS_SUCCESS;
}


static long bcm_char_ioctl(struct file *filp, UINT cmd, ULONG arg)
{
	struct bcm_tarang_data *pTarang = filp->private_data;
	void __user *argp = (void __user *)arg;
	struct bcm_mini_adapter *ad = pTarang->Adapter;
	INT Status = STATUS_FAILURE;

	BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
			"Parameters Passed to control IOCTL cmd=0x%X arg=0x%lX",
			cmd, arg);

	if (_IOC_TYPE(cmd) != BCM_IOCTL)
		return -EFAULT;
	if (_IOC_DIR(cmd) & _IOC_READ)
		Status = !access_ok(VERIFY_WRITE, argp, _IOC_SIZE(cmd));
	else if (_IOC_DIR(cmd) & _IOC_WRITE)
		Status = !access_ok(VERIFY_READ, argp, _IOC_SIZE(cmd));
	else if (_IOC_NONE == (_IOC_DIR(cmd) & _IOC_NONE))
		Status = STATUS_SUCCESS;

	if (Status)
		return -EFAULT;

	if (ad->device_removed)
		return -EFAULT;

	if (false == ad->fw_download_done) {
		switch (cmd) {
		case IOCTL_MAC_ADDR_REQ:
		case IOCTL_LINK_REQ:
		case IOCTL_CM_REQUEST:
		case IOCTL_SS_INFO_REQ:
		case IOCTL_SEND_CONTROL_MESSAGE:
		case IOCTL_IDLE_REQ:
		case IOCTL_BCM_GPIO_SET_REQUEST:
		case IOCTL_BCM_GPIO_STATUS_REQUEST:
			return -EACCES;
		default:
			break;
		}
	}

	Status = vendorextnIoctl(ad, cmd, arg);
	if (Status != CONTINUE_COMMON_PATH)
		return Status;

	switch (cmd) {
	/* Rdms for Swin Idle... */
	case IOCTL_BCM_REGISTER_READ_PRIVATE:
		Status = bcm_char_ioctl_reg_read_private(argp, ad);
		return Status;

	case IOCTL_BCM_REGISTER_WRITE_PRIVATE:
		Status = bcm_char_ioctl_reg_write_private(argp, ad);
		return Status;

	case IOCTL_BCM_REGISTER_READ:
	case IOCTL_BCM_EEPROM_REGISTER_READ:
		Status = bcm_char_ioctl_eeprom_reg_read(argp, ad);
		return Status;

	case IOCTL_BCM_REGISTER_WRITE:
	case IOCTL_BCM_EEPROM_REGISTER_WRITE:
		Status = bcm_char_ioctl_eeprom_reg_write(argp, ad, cmd);
		return Status;

	case IOCTL_BCM_GPIO_SET_REQUEST:
		Status = bcm_char_ioctl_gpio_set_request(argp, ad);
		return Status;

	case BCM_LED_THREAD_STATE_CHANGE_REQ:
		Status = bcm_char_ioctl_led_thread_state_change_req(argp,
								    ad);
		return Status;

	case IOCTL_BCM_GPIO_STATUS_REQUEST:
		Status = bcm_char_ioctl_gpio_status_request(argp, ad);
		return Status;

	case IOCTL_BCM_GPIO_MULTI_REQUEST:
		Status = bcm_char_ioctl_gpio_multi_request(argp, ad);
		return Status;

	case IOCTL_BCM_GPIO_MODE_REQUEST:
		Status = bcm_char_ioctl_gpio_mode_request(argp, ad);
		return Status;

	case IOCTL_MAC_ADDR_REQ:
	case IOCTL_LINK_REQ:
	case IOCTL_CM_REQUEST:
	case IOCTL_SS_INFO_REQ:
	case IOCTL_SEND_CONTROL_MESSAGE:
	case IOCTL_IDLE_REQ:
		Status = bcm_char_ioctl_misc_request(argp, ad);
		return Status;

	case IOCTL_BCM_BUFFER_DOWNLOAD_START:
		Status = bcm_char_ioctl_buffer_download_start(ad);
		return Status;

	case IOCTL_BCM_BUFFER_DOWNLOAD:
		Status = bcm_char_ioctl_buffer_download(argp, ad);
		return Status;

	case IOCTL_BCM_BUFFER_DOWNLOAD_STOP:
		Status = bcm_char_ioctl_buffer_download_stop(argp, ad);
		return Status;


	case IOCTL_BE_BUCKET_SIZE:
		Status = 0;
		if (get_user(ad->BEBucketSize,
			     (unsigned long __user *)arg))
			Status = -EFAULT;
		break;

	case IOCTL_RTPS_BUCKET_SIZE:
		Status = 0;
		if (get_user(ad->rtPSBucketSize,
			     (unsigned long __user *)arg))
			Status = -EFAULT;
		break;

	case IOCTL_CHIP_RESET:
		Status = bcm_char_ioctl_chip_reset(ad);
		return Status;

	case IOCTL_QOS_THRESHOLD:
		Status = bcm_char_ioctl_qos_threshold(arg, ad);
		return Status;

	case IOCTL_DUMP_PACKET_INFO:
		DumpPackInfo(ad);
		DumpPhsRules(&ad->stBCMPhsContext);
		Status = STATUS_SUCCESS;
		break;

	case IOCTL_GET_PACK_INFO:
		if (copy_to_user(argp, &ad->PackInfo,
				 sizeof(struct bcm_packet_info)*NO_OF_QUEUES))
			return -EFAULT;
		Status = STATUS_SUCCESS;
		break;

	case IOCTL_BCM_SWITCH_TRANSFER_MODE:
		Status = bcm_char_ioctl_switch_transfer_mode(argp, ad);
		return Status;

	case IOCTL_BCM_GET_DRIVER_VERSION:
		Status = bcm_char_ioctl_get_driver_version(argp);
		return Status;

	case IOCTL_BCM_GET_CURRENT_STATUS:
		Status = bcm_char_ioctl_get_current_status(argp, ad);
		return Status;

	case IOCTL_BCM_SET_MAC_TRACING:
		Status = bcm_char_ioctl_set_mac_tracing(argp, ad);
		return Status;

	case IOCTL_BCM_GET_DSX_INDICATION:
		Status = bcm_char_ioctl_get_dsx_indication(argp, ad);
		return Status;

	case IOCTL_BCM_GET_HOST_MIBS:
		Status = bcm_char_ioctl_get_host_mibs(argp, ad, pTarang);
		return Status;

	case IOCTL_BCM_WAKE_UP_DEVICE_FROM_IDLE:
		if ((false == ad->bTriedToWakeUpFromlowPowerMode) &&
				(TRUE == ad->IdleMode)) {
			ad->usIdleModePattern = ABORT_IDLE_MODE;
			ad->bWakeUpDevice = TRUE;
			wake_up(&ad->process_rx_cntrlpkt);
		}

		Status = STATUS_SUCCESS;
		break;

	case IOCTL_BCM_BULK_WRM:
		Status = bcm_char_ioctl_bulk_wrm(argp, ad, cmd);
		return Status;

	case IOCTL_BCM_GET_NVM_SIZE:
		Status = bcm_char_ioctl_get_nvm_size(argp, ad);
		return Status;

	case IOCTL_BCM_CAL_INIT:
		Status = bcm_char_ioctl_cal_init(argp, ad);
		return Status;

	case IOCTL_BCM_SET_DEBUG:
		Status = bcm_char_ioctl_set_debug(argp, ad);
		return Status;

	case IOCTL_BCM_NVM_READ:
	case IOCTL_BCM_NVM_WRITE:
		Status = bcm_char_ioctl_nvm_rw(argp, ad, cmd);
		return Status;

	case IOCTL_BCM_FLASH2X_SECTION_READ:
		Status = bcm_char_ioctl_flash2x_section_read(argp, ad);
		return Status;

	case IOCTL_BCM_FLASH2X_SECTION_WRITE:
		Status = bcm_char_ioctl_flash2x_section_write(argp, ad);
		return Status;

	case IOCTL_BCM_GET_FLASH2X_SECTION_BITMAP:
		Status = bcm_char_ioctl_flash2x_section_bitmap(argp, ad);
		return Status;

	case IOCTL_BCM_SET_ACTIVE_SECTION:
		Status = bcm_char_ioctl_set_active_section(argp, ad);
		return Status;

	case IOCTL_BCM_IDENTIFY_ACTIVE_SECTION:
		/* Right Now we are taking care of only DSD */
		ad->bAllDSDWriteAllow = false;
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
				"IOCTL_BCM_IDENTIFY_ACTIVE_SECTION called");
		Status = STATUS_SUCCESS;
		break;

	case IOCTL_BCM_COPY_SECTION:
		Status = bcm_char_ioctl_copy_section(argp, ad);
		return Status;

	case IOCTL_BCM_GET_FLASH_CS_INFO:
		Status = bcm_char_ioctl_get_flash_cs_info(argp, ad);
		return Status;

	case IOCTL_BCM_SELECT_DSD:
		Status = bcm_char_ioctl_select_dsd(argp, ad);
		return Status;

	case IOCTL_BCM_NVM_RAW_READ:
		Status = bcm_char_ioctl_nvm_raw_read(argp, ad);
		return Status;

	case IOCTL_BCM_CNTRLMSG_MASK:
		Status = bcm_char_ioctl_cntrlmsg_mask(argp, ad, pTarang);
		return Status;

	case IOCTL_BCM_GET_DEVICE_DRIVER_INFO:
		Status = bcm_char_ioctl_get_device_driver_info(argp, ad);
		return Status;

	case IOCTL_BCM_TIME_SINCE_NET_ENTRY:
		Status = bcm_char_ioctl_time_since_net_entry(argp, ad);
		return Status;

	case IOCTL_CLOSE_NOTIFICATION:
		BCM_DEBUG_PRINT(ad, DBG_TYPE_OTHERS, OSAL_DBG, DBG_LVL_ALL,
				"IOCTL_CLOSE_NOTIFICATION");
		break;

	default:
		pr_info(DRV_NAME ": unknown ioctl cmd=%#x\n", cmd);
		Status = STATUS_FAILURE;
		break;
	}
	return Status;
}


static const struct file_operations bcm_fops = {
	.owner    = THIS_MODULE,
	.open     = bcm_char_open,
	.release  = bcm_char_release,
	.read     = bcm_char_read,
	.unlocked_ioctl    = bcm_char_ioctl,
	.llseek = no_llseek,
};

int register_control_device_interface(struct bcm_mini_adapter *ad)
{

	if (ad->major > 0)
		return ad->major;

	ad->major = register_chrdev(0, DEV_NAME, &bcm_fops);
	if (ad->major < 0) {
		pr_err(DRV_NAME ": could not created character device\n");
		return ad->major;
	}

	ad->pstCreatedClassDevice = device_create(bcm_class, NULL,
						       MKDEV(ad->major, 0),
						       ad, DEV_NAME);

	if (IS_ERR(ad->pstCreatedClassDevice)) {
		pr_err(DRV_NAME ": class device create failed\n");
		unregister_chrdev(ad->major, DEV_NAME);
		return PTR_ERR(ad->pstCreatedClassDevice);
	}

	return 0;
}

void unregister_control_device_interface(struct bcm_mini_adapter *ad)
{
	if (ad->major > 0) {
		device_destroy(bcm_class, MKDEV(ad->major, 0));
		unregister_chrdev(ad->major, DEV_NAME);
	}
}

