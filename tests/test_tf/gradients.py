#
# <one line to give the library's name and an idea of what it does.>
# Copyright (C) 2017  Aetf <aetf@unlimitedcodeworks.xyz>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#
import numpy as np
import tensorflow as tf


def _product(t):
    if isinstance(t, int):
        return t
    else:
        y = 1
        for x in t:
            y *= x
        return y


def _extra_feeds(extra_feed_dict, new_feeds):
    if not extra_feed_dict:
        return new_feeds
    r = {}
    r.update(extra_feed_dict)
    r.update(new_feeds)
    return r


def _compute_theoretical_jacobian(x, x_shape, x_data, dy, dy_shape, dx,
                                  extra_feed_dict):
    """Computes the theoretical Jacobian for dy/dx.

    Computes the theoretical Jacobian using the ops generated by
    compute_gradient().

    Args:
      x: the tensor "x".
      x_shape: the dimensions of x as a tuple or an array of ints.
      x_data: a numpy parray as the input data for x
      dy: the tensor "dy".
      dy_shape: the dimensions of dy as a tuple or an array of ints.
      dx: Tensor or IndexedSlices representing dx
      extra_feed_dict: dict that allows fixing specified tensor values
        during the jacobian calculation.

    Returns:
      A 2-d numpy array representing the Jacobian for dy/dx. It has "x_size" rows
      and "dy_size" columns where "x_size" is the number of elements in x and
      "dy_size" is the number of elements in dy.

    Raises:
      ValueError: If `dy` is empty but the gradient is nonzero.
    """
    # Complex vectors are treated as vectors of twice as many reals.
    if x.dtype.is_complex:
        x_shape = tuple(x_shape) + (2,)
    dy_factor = 2 if dy.dtype.is_complex else 1

    # To compute the jacobian, we treat x and y as one-dimensional vectors.
    x_size = _product(x_shape)
    x_val_size = _product(x_shape[1:])  # This is used for sparse gradients
    dy_size = _product(dy_shape) * dy_factor

    # Allocate 2-D Jacobian, with x dimensions smashed into the first
    # dimension and y dimensions smashed into the second.
    jacobian = np.zeros((x_size, dy_size),
                        dtype=x.dtype.real_dtype.as_numpy_dtype)

    # For each of the entry of dy, we set this to be 1 and
    # everything else to be 0 and compute the backprop -- this will give us one
    # one column of the Jacobian matrix.
    dy_data = np.zeros(dy_shape, dtype=dy.dtype.as_numpy_dtype)
    dy_data_flat = dy_data.ravel().view(dy.dtype.real_dtype.as_numpy_dtype)
    sess = tf.get_default_session()
    for col in range(dy_size):
        dy_data_flat[col] = 1
        if isinstance(dx, tf.IndexedSlices):
            backprop_indices, backprop_values = sess.run(
                [dx.indices, dx.values],
                feed_dict=_extra_feeds(extra_feed_dict, {x: x_data, dy: dy_data}))
            for i, v in zip(backprop_indices, backprop_values):
                r_begin = i * x_val_size
                r_end = r_begin + x_val_size
                jacobian[r_begin:r_end, col] += v.flat
        else:
            assert isinstance(dx, tf.Tensor), "dx = " + str(dx)
            backprop = sess.run(
                dx, feed_dict=_extra_feeds(extra_feed_dict, {x: x_data, dy: dy_data}))
            jacobian[:, col] = backprop.ravel().view(jacobian.dtype)
        dy_data_flat[col] = 0

    # If the output is empty, run the gradients at least once and make sure
    # they produce zeros.
    if not dy_size:
        backprop = sess.run(
            dx, feed_dict=_extra_feeds(extra_feed_dict, {x: x_data, dy: dy_data}))
        if backprop.shape != x_data.shape:
            raise ValueError("Empty gradient has wrong shape: expected %s, got %s" %
                             (x_data.shape, backprop.shape))
        if np.any(backprop):
            raise ValueError("Empty tensor with nonzero gradients")

    return jacobian


def _compute_gradient(x,
                      x_shape,
                      dx,
                      y,
                      y_shape,
                      dy,
                      x_init_value=None,
                      delta=1e-3,
                      extra_feed_dict=None):
    """Computes the jacobian."""
    t = tf.as_dtype(x.dtype)
    allowed_types = [tf.float16, tf.float32, tf.float64,
                     tf.complex64, tf.complex128]
    assert t.base_dtype in allowed_types, "Don't support type %s for x" % t.name
    t2 = tf.as_dtype(y.dtype)
    assert t2.base_dtype in allowed_types, "Don't support type %s for y" % t2.name

    if x_init_value is not None:
        i_shape = list(x_init_value.shape)
        assert(list(x_shape) == i_shape), "x_shape = %s, init_data shape = %s" % (
            x_shape, i_shape)
        x_data = x_init_value
    else:
        x_data = np.random.random_sample(x_shape).astype(t.as_numpy_dtype)
        if t.is_complex:
            x_data.imag = np.random.random_sample(x_shape)

    jacob_t = _compute_theoretical_jacobian(
        x, x_shape, x_data, dy, y_shape, dx, extra_feed_dict=extra_feed_dict)
    return jacob_t


def _compute_dx_and_dy(x, y, y_shape):
    """Returns a node to compute gradient of x wrt y."""
    # We make up a dy so that we can compute the gradients. We don't really use
    # the value of dy -- we will always feed it. We need to add an identity node
    # so that we can always feed it properly. Otherwise, for the Add operation,
    # dx is the same as dy and we cannot fetch the tensor that we are feeding.
    with x.graph.as_default():
        dy_orig = tf.constant(1, shape=y_shape, dtype=y.dtype)
        dy = tf.identity(dy_orig)
    # We compute the gradients for x wrt. y
    grads = tf.gradients(y, x, dy)
    assert len(grads) == 1
    return grads[0], dy_orig


def _compute_gradient_list(x,
                           x_shape,
                           y,
                           y_shape,
                           x_init_value=None,
                           delta=1e-3,
                           init_targets=None,
                           extra_feed_dict=None):
    """Compute gradients for a list of x values."""
    assert isinstance(x, list)
    dx, dy = zip(*[_compute_dx_and_dy(xi, y, y_shape) for xi in x])

    if init_targets is not None:
        assert isinstance(init_targets, (list, tuple))
        for init in init_targets:
            init.run()
    if x_init_value is None:
        x_init_value = [None] * len(x)
    ret = [_compute_gradient(xi, x_shapei, dxi, y, y_shape, dyi, x_init_valuei,
                             delta, extra_feed_dict=extra_feed_dict)
           for xi, x_shapei, dxi, dyi, x_init_valuei in zip(x, x_shape, dx, dy,
                                                            x_init_value)]
    return ret


def compute_gradient(x,
                     x_shape,
                     y,
                     y_shape,
                     x_init_value=None,
                     delta=1e-3,
                     init_targets=None,
                     extra_feed_dict=None):
    """Computes and returns the gradient of x wrt y (Jacobian).

    If `x` or `y` is complex, the Jacobian will still be real but the
    corresponding Jacobian dimension(s) will be twice as large.  This is required
    even if both input and output is complex since TensorFlow graphs are not
    necessarily holomorphic, and may have gradients not expressible as complex
    numbers.  For example, if `x` is complex with shape `[m]` and `y` is complex
    with shape `[n]`, each Jacobian `J` will have shape `[m * 2, n * 2]` with

        J[:m, :n] = d(Re y)/d(Re x)
        J[:m, n:] = d(Im y)/d(Re x)
        J[m:, :n] = d(Re y)/d(Im x)
        J[m:, n:] = d(Im y)/d(Im x)

    Args:
      x: a tensor or list of tensors
      x_shape: the dimensions of x as a tuple or an array of ints. If x is a list,
      then this is the list of shapes.
      y: a tensor
      y_shape: the dimensions of y as a tuple or an array of ints.
      x_init_value: (optional) a numpy array of the same shape as "x"
        representing the initial value of x. If x is a list, this should be a list
        of numpy arrays.  If this is none, the function will pick a random tensor
        as the initial value.
      delta: (optional) the amount of perturbation.
      init_targets: list of targets to run to initialize model params.
        TODO(mrry): remove this argument.
      extra_feed_dict: dict that allows fixing specified tensor values
        during the Jacobian calculation.

    Returns:
      2-d numpy array representing the Jacobian for dy/dx.
      It has "x_size" rows and "y_size" columns
      where "x_size" is the number of elements in x and "y_size" is the
      number of elements in y. If x is a list, returns a list of numpy arrays.
    """
    if extra_feed_dict is None:
        extra_feed_dict = {}

    if isinstance(x, list):
        return _compute_gradient_list(x, x_shape, y, y_shape, x_init_value, delta,
                                      init_targets, extra_feed_dict=extra_feed_dict)
    else:
        if init_targets is not None:
            assert isinstance(init_targets, (list, tuple))
            for init in init_targets:
                init.run()
        dx, dy = _compute_dx_and_dy(x, y, y_shape)
        ret = _compute_gradient(x, x_shape, dx, y, y_shape, dy, x_init_value, delta,
                                extra_feed_dict=extra_feed_dict)
        return ret
