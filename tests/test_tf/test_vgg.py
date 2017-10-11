from __future__ import print_function

import unittest
from datetime import datetime
from timeit import default_timer

import numpy as np
import tensorflow as tf

from parameterized import parameterized

from . import run_on_rpc_and_cpu, run_on_rpc_and_gpu, run_on_sessions, run_on_devices
from . import networks, datasets
from .lib import tfhelper


def run_vgg(vgg, sess, input_data, batch_size=100):
    if vgg is None:
        raise ValueError('VGG is None!!')

    images, labels, num_classes = input_data(batch_size=batch_size, batch_num=100)
    train_mode = tf.placeholder(tf.bool)

    vgg.build(images, train_mode)

    # print number of variables used: 143667240 variables, i.e. ideal size = 548MB
    print('Total number of variables: {}'.format(vgg.get_var_count()))

    cross_entropy = tf.reduce_mean(
        tf.nn.sparse_softmax_cross_entropy_with_logits(labels=labels, logits=vgg.prob))
    train_step = tf.train.AdamOptimizer(1e-4).minimize(cross_entropy)

    # vgg.prob: [batch_size, 1000]
    # labels: [batch_size,]
    correct_prediction = tf.equal(tf.argmax(vgg.prob, 1), tf.argmax(labels))
    accuracy = tf.reduce_mean(tf.cast(correct_prediction, tf.float32))

    with tfhelper.initialized_scope(sess) as coord:
        speeds = []
        inbetween = []
        last_end_time = 0
        JCT = default_timer()
        for i in range(5):
            if coord.should_stop():
                break
            print("{}: Start running step {}".format(datetime.now(), i))
            start_time = default_timer()
            _, loss_value = sess.run([train_step, cross_entropy], feed_dict={train_mode: True})
            end_time = default_timer()

            if last_end_time > 0:
                inbetween.append(start_time - last_end_time)
            last_end_time = end_time

            duration = end_time - start_time
            examples_per_sec = batch_size / duration
            sec_per_batch = float(duration)
            speeds.append(sec_per_batch)
            fmt_str = '{}: step {}, loss = {:.2f} ({:.1f} examples/sec; {:.3f} sec/batch'
            print(fmt_str.format(datetime.now(), i, loss_value, examples_per_sec, sec_per_batch))

        print('Average %.3f sec/batch' % np.average(speeds))
        print('Average %.6f sec spent between batches' % np.average(inbetween))
        JCT = default_timer() - JCT
        print('Training time is %.3f sec' % JCT)

        print('{}: Start final eva'.format(datetime.now()))
        start_time = default_timer()
        final_acc = sess.run(accuracy, feed_dict={train_mode: False})
        duration = default_timer() - start_time
        print('Final eval takes %.3f sec' % duration)

    return final_acc


class VGGCaseBase(unittest.TestCase):
    def _vgg(self):
        return None

    def _config(self, **kwargs):
        return tf.ConfigProto()

    @parameterized.expand([(25,), (50,), (100,)])
    def test_gpu(self, batch_size):
        def func():
            def input_data(*a, **kw):
                return datasets.fake_data(*a, height=224, width=224, num_classes=1000, **kw)
            sess = tf.get_default_session()
            return run_vgg(self._vgg(), sess, input_data, batch_size=batch_size)

        config = self._config(batch_size=batch_size)
        config.allow_soft_placement = True
        run_on_devices(func, '/device:GPU:0', config=config)

    def test_cpu(self):
        def func():
            def input_data(*a, **kw):
                return datasets.fake_data(*a, height=224, width=224, num_classes=1000, **kw)
            sess = tf.get_default_session()
            return run_vgg(self._vgg(), sess, input_data)

        run_on_devices(func, '/device:CPU:0')

    @parameterized.expand([(25,), (50,), (100,)])
    def test_rpc_only(self, batch_size):
        def func():
            def input_data(*a, **kw):
                return datasets.fake_data(*a, height=224, width=224, num_classes=1000, **kw)
            sess = tf.get_default_session()
            return run_vgg(self._vgg(), sess, input_data, batch_size=batch_size)

        run_on_sessions(func, 'zrpc://tcp://127.0.0.1:5501', config=self._config(batch_size=batch_size))

    def test_fake_data(self):
        def func():
            def input_data(*a, **kw):
                return datasets.fake_data(*a, height=224, width=224, num_classes=1000, **kw)
            sess = tf.get_default_session()
            return run_vgg(self._vgg(), sess, input_data)

        actual, expected = run_on_rpc_and_cpu(func, config=tf.ConfigProto(allow_soft_placement=True))
        self.assertEquals(actual, expected)

    @unittest.skip("Not yet implemented")
    def test_fake_data_gpu(self):
        def func():
            def input_data(*a, **kw):
                return datasets.fake_data(*a, height=224, width=224, num_classes=1000, **kw)
            sess = tf.get_default_session()
            return run_vgg(self._vgg(), sess, input_data)

        actual, expected = run_on_rpc_and_gpu(func)
        self.assertEquals(actual, expected)

    def test_flowers(self):
        def func():
            def input_data(*a, **kw):
                return datasets.flowers_data(*a, height=224, width=224, **kw)
            sess = tf.get_default_session()
            return run_vgg(self._vgg(), sess, input_data)

        actual, expected = run_on_rpc_and_cpu(func)
        self.assertEquals(actual, expected)

    @unittest.skip("Skip distributed runtime")
    def test_dist(self):
        def func():
            def input_data(*a, **kw):
                return datasets.fake_data(*a, height=224, width=224, num_classes=1000, **kw)
            sess = tf.get_default_session()
            return run_vgg(self._vgg(), sess, input_data)

        run_on_sessions(func, 'grpc://localhost:2222')

    @unittest.skip("Skip distributed runtime")
    def test_dist_gpu(self):
        def func():
            def input_data(*a, **kw):
                return datasets.fake_data(*a, height=224, width=224, num_classes=1000, **kw)
            sess = tf.get_default_session()
            return run_vgg(self._vgg(), sess, input_data)
        run_on_devices(func, '/job:local/task:0/device:GPU:0', target='grpc://localhost:2222',
                       config=tf.ConfigProto(allow_soft_placement=True))


class TestVgg11(VGGCaseBase):
    def _vgg(self):
        return networks.Vgg11Trainable()


class TestVgg16(VGGCaseBase):
    def _vgg(self):
        return networks.Vgg16Trainable()

    def _config(self, **kwargs):
        memusages = {
            25: (6935520748 - 1661494764, 1661494764),
            50: (10211120620 - 1662531248, 1662531248),
            100: (11494955340, 1.67e9),
        }
        batch_size = kwargs.get('batch_size', 100)

        config = tf.ConfigProto()
        config.zmq_options.resource_map.temporary['MEMORY:GPU'] = memusages[batch_size][0]
        config.zmq_options.resource_map.persistant['MEMORY:GPU'] = memusages[batch_size][1]
        return config


class TestVgg19(VGGCaseBase):
    def _vgg(self):
        return networks.Vgg19Trainable()


del VGGCaseBase


if __name__ == '__main__':
    unittest.main()
