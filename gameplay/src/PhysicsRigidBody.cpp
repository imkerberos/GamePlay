#include "Base.h"
#include "Game.h"
#include "Image.h"
#include "PhysicsController.h"
#include "PhysicsMotionState.h"
#include "PhysicsRigidBody.h"

namespace gameplay
{

// Internal values used for creating mesh, heightfield, and capsule rigid bodies.
#define SHAPE_MESH ((PhysicsRigidBody::ShapeType)(PhysicsRigidBody::SHAPE_NONE + 1))
#define SHAPE_HEIGHTFIELD ((PhysicsRigidBody::ShapeType)(PhysicsRigidBody::SHAPE_NONE + 2))
#define SHAPE_CAPSULE ((PhysicsRigidBody::ShapeType)(PhysicsRigidBody::SHAPE_NONE + 3))

// Helper function for calculating heights from heightmap (image) or heightfield data.
static float calculateHeight(float* data, unsigned int width, unsigned int height, float x, float y);

PhysicsRigidBody::PhysicsRigidBody(Node* node, PhysicsRigidBody::ShapeType type, float mass, 
    float friction, float restitution, float linearDamping, float angularDamping)
        : _shape(NULL), _body(NULL), _node(node), _angularVelocity(NULL),
        _anisotropicFriction(NULL), _gravity(NULL), _linearVelocity(NULL), _vertexData(NULL),
        _indexData(NULL), _heightfieldData(NULL), _inverse(NULL), _inverseIsDirty(true)
{
    // Get the node's world scale (we need to apply this during creation since rigid bodies don't scale dynamically).
    Vector3 scale;
    node->getWorldMatrix().getScale(&scale);

    switch (type)
    {
        case SHAPE_BOX:
        {
            const BoundingBox& box = node->getModel()->getMesh()->getBoundingBox();
            _shape = Game::getInstance()->getPhysicsController()->createBox(box.min, box.max, scale);
            break;
        }
        case SHAPE_SPHERE:
        {
            const BoundingSphere& sphere = node->getModel()->getMesh()->getBoundingSphere();
            _shape = Game::getInstance()->getPhysicsController()->createSphere(sphere.radius, scale);
            break;
        }
        case SHAPE_MESH:
        {
            _shape = Game::getInstance()->getPhysicsController()->createMesh(this, scale);
            break;
        }
    }

    // Use the center of the bounding sphere as the center of mass offset.
    Vector3 c(node->getModel()->getMesh()->getBoundingSphere().center);
    c.x *= scale.x;
    c.y *= scale.y;
    c.z *= scale.z;
    c.negate();

    // Create the Bullet rigid body (we don't apply center of mass offsets on mesh rigid bodies).
    if (c.lengthSquared() > MATH_EPSILON && type != SHAPE_MESH)
        _body = createRigidBodyInternal(_shape, mass, node, friction, restitution, linearDamping, angularDamping, &c);
    else
        _body = createRigidBodyInternal(_shape, mass, node, friction, restitution, linearDamping, angularDamping);

    // Add the rigid body to the physics world.
    Game::getInstance()->getPhysicsController()->addCollisionObject(this);
}

PhysicsRigidBody::PhysicsRigidBody(Node* node, Image* image, float mass,
    float friction, float restitution, float linearDamping, float angularDamping)
        : _shape(NULL), _body(NULL), _node(node), _angularVelocity(NULL),
        _anisotropicFriction(NULL), _gravity(NULL), _linearVelocity(NULL), _vertexData(NULL),
        _indexData(NULL), _heightfieldData(NULL), _inverse(NULL), _inverseIsDirty(true)
{
    // Get the width, length and minimum and maximum height of the heightfield.
    const BoundingBox& box = node->getModel()->getMesh()->getBoundingBox();
    float width = box.max.x - box.min.x;
    float minHeight = box.min.y;
    float maxHeight = box.max.y;
    float length = box.max.z - box.min.z;

    // Get the size in bytes of a pixel (we ensure that the image's
    // pixel format is actually supported before calling this constructor).
    unsigned int pixelSize = 0;
    switch (image->getFormat())
    {
        case Image::RGB:
            pixelSize = 3;
            break;
        case Image::RGBA:
            pixelSize = 4;
            break;
    }

    // Calculate the heights for each pixel.
    float* data = new float[image->getWidth() * image->getHeight()];
    for (unsigned int x = 0; x < image->getWidth(); x++)
    {
        for (unsigned int y = 0; y < image->getHeight(); y++)
        {
            data[x + y * image->getWidth()] = ((((float)image->getData()[(x + y * image->getHeight()) * pixelSize + 0]) +
                ((float)image->getData()[(x + y * image->getHeight()) * pixelSize + 1]) +
                ((float)image->getData()[(x + y * image->getHeight()) * pixelSize + 2])) / 768.0f) * (maxHeight - minHeight) + minHeight;
        }
    }

    // Generate the heightmap data needed for physics (one height per world unit).
    unsigned int sizeWidth = width;
    unsigned int sizeHeight = length;
    _width = sizeWidth + 1;
    _height = sizeHeight + 1;
    _heightfieldData = new float[_width * _height];
    unsigned int heightIndex = 0;
    float widthImageFactor = (float)(image->getWidth() - 1) / sizeWidth;
    float heightImageFactor = (float)(image->getHeight() - 1) / sizeHeight;
    float x = 0.0f;
    float z = 0.0f;
    for (unsigned int row = 0, z = 0.0f; row <= sizeHeight; row++, z += 1.0f)
    {
        for (unsigned int col = 0, x = 0.0f; col <= sizeWidth; col++, x += 1.0f)
        {
            heightIndex = row * _width + col;
            _heightfieldData[heightIndex] = calculateHeight(data, image->getWidth(), image->getHeight(), x * widthImageFactor, (sizeHeight - z) * heightImageFactor);
        }
    }
    SAFE_DELETE_ARRAY(data);

    // Create the heightfield collision shape.
    _shape = Game::getInstance()->getPhysicsController()->createHeightfield(_width, _height, _heightfieldData, minHeight, maxHeight);

    // Offset the heightmap's center of mass according to the way that Bullet calculates the origin 
    // of its heightfield collision shape; see documentation for the btHeightfieldTerrainShape for more info.
    Vector3 s;
    node->getWorldMatrix().getScale(&s);
    Vector3 c (0.0f, -(maxHeight - (0.5f * (maxHeight - minHeight))) / s.y, 0.0f);

    // Create the Bullet rigid body.
    if (c.lengthSquared() > MATH_EPSILON)
        _body = createRigidBodyInternal(_shape, mass, node, friction, restitution, linearDamping, angularDamping, &c);
    else
        _body = createRigidBodyInternal(_shape, mass, node, friction, restitution, linearDamping, angularDamping);

    // Add the rigid body to the physics world.
    Game::getInstance()->getPhysicsController()->addCollisionObject(this);

    // Add the rigid body as a listener on the node's transform.
    _node->addListener(this);
}

PhysicsRigidBody::PhysicsRigidBody(Node* node, float radius, float height, float mass, float friction,
    float restitution, float linearDamping, float angularDamping)
        : _shape(NULL), _body(NULL), _node(node), _angularVelocity(NULL),
        _anisotropicFriction(NULL), _gravity(NULL), _linearVelocity(NULL), _vertexData(NULL),
        _indexData(NULL), _heightfieldData(NULL), _inverse(NULL), _inverseIsDirty(true)
{
    // Get the node's world scale (we need to apply this during creation since rigid bodies don't scale dynamically).
    Vector3 scale;
    node->getWorldMatrix().getScale(&scale);

    // Create the capsule collision shape.
    _shape = Game::getInstance()->getPhysicsController()->createCapsule(radius, height);

    // Use the center of the bounding sphere as the center of mass offset.
    Vector3 c(node->getModel()->getMesh()->getBoundingSphere().center);
    c.x *= scale.x;
    c.y *= scale.y;
    c.z *= scale.z;
    c.negate();

    // Create the Bullet rigid body.
    if (c.lengthSquared() > MATH_EPSILON)
        _body = createRigidBodyInternal(_shape, mass, node, friction, restitution, linearDamping, angularDamping, &c);
    else
        _body = createRigidBodyInternal(_shape, mass, node, friction, restitution, linearDamping, angularDamping);

    // Add the rigid body to the physics world.
    Game::getInstance()->getPhysicsController()->addCollisionObject(this);
}

PhysicsRigidBody::~PhysicsRigidBody()
{
    // Clean up all constraints linked to this rigid body.
    PhysicsConstraint* ptr = NULL;
    while (_constraints.size() > 0)
    {
        ptr = _constraints.back();
        _constraints.pop_back();
        SAFE_DELETE(ptr);
    }

    Game::getInstance()->getPhysicsController()->removeCollisionObject(this);

    // Clean up the rigid body and its related objects.
    if (_body)
    {
        if (_body->getMotionState())
            delete _body->getMotionState();
        SAFE_DELETE(_body);
    }

    SAFE_DELETE(_angularVelocity);
    SAFE_DELETE(_anisotropicFriction);
    SAFE_DELETE(_gravity);
    SAFE_DELETE(_linearVelocity);
    SAFE_DELETE_ARRAY(_vertexData);
    for (unsigned int i = 0; i < _indexData.size(); i++)
    {
        SAFE_DELETE_ARRAY(_indexData[i]);
    }
    SAFE_DELETE_ARRAY(_heightfieldData);
    SAFE_DELETE(_inverse);
}

Node* PhysicsRigidBody::getNode() const
{
    return _node;
}

PhysicsCollisionObject::Type PhysicsRigidBody::getType() const
{
    return PhysicsCollisionObject::RIGID_BODY;
}

btCollisionObject* PhysicsRigidBody::getCollisionObject() const
{
    return _body;
}

btCollisionShape* PhysicsRigidBody::getCollisionShape() const
{
    return _shape;
}

void PhysicsRigidBody::applyForce(const Vector3& force, const Vector3* relativePosition)
{
    // If the force is significant enough, activate the rigid body 
    // to make sure that it isn't sleeping and apply the force.
    if (force.lengthSquared() > MATH_EPSILON)
    {
        _body->activate();
        if (relativePosition)
            _body->applyForce(BV(force), BV(*relativePosition));
        else
            _body->applyCentralForce(BV(force));
    }
}

void PhysicsRigidBody::applyImpulse(const Vector3& impulse, const Vector3* relativePosition)
{
    // If the impulse is significant enough, activate the rigid body 
    // to make sure that it isn't sleeping and apply the impulse.
    if (impulse.lengthSquared() > MATH_EPSILON)
    {
        _body->activate();

        if (relativePosition)
        {
            _body->applyImpulse(BV(impulse), BV(*relativePosition));
        }
        else
            _body->applyCentralImpulse(BV(impulse));
    }
}

void PhysicsRigidBody::applyTorque(const Vector3& torque)
{
    // If the torque is significant enough, activate the rigid body 
    // to make sure that it isn't sleeping and apply the torque.
    if (torque.lengthSquared() > MATH_EPSILON)
    {
        _body->activate();
        _body->applyTorque(BV(torque));
    }
}

void PhysicsRigidBody::applyTorqueImpulse(const Vector3& torque)
{
    // If the torque impulse is significant enough, activate the rigid body 
    // to make sure that it isn't sleeping and apply the torque impulse.
    if (torque.lengthSquared() > MATH_EPSILON)
    {
        _body->activate();
        _body->applyTorqueImpulse(BV(torque));
    }
}

PhysicsRigidBody* PhysicsRigidBody::create(Node* node, const char* filePath)
{
    assert(filePath);

    // Load the rigid body properties from file.
    Properties* properties = Properties::create(filePath);
    assert(properties);
    if (properties == NULL)
    {
        WARN_VARG("Failed to load rigid body file: %s", filePath);
        return NULL;
    }

    PhysicsRigidBody* body = create(node, properties->getNextNamespace());
    SAFE_DELETE(properties);

    return body;
}

PhysicsRigidBody* PhysicsRigidBody::create(Node* node, Properties* properties)
{
    // Check if the properties is valid and has a valid namespace.
    assert(properties);
    if (!properties || !(strcmp(properties->getNamespace(), "rigidbody") == 0))
    {
        WARN("Failed to load rigid body from properties object: must be non-null object and have namespace equal to \'rigidbody\'.");
        return NULL;
    }

    // Set values to their defaults.
    PhysicsRigidBody::ShapeType type = PhysicsRigidBody::SHAPE_NONE;
    float mass = 0.0;
    float friction = 0.5;
    float restitution = 0.0;
    float linearDamping = 0.0;
    float angularDamping = 0.0;
    bool kinematic = false;
    Vector3* gravity = NULL;
    Vector3* anisotropicFriction = NULL;
    const char* imagePath = NULL;
    float radius = -1.0f;
    float height = -1.0f;

    // Load the defined properties.
    properties->rewind();
    const char* name;
    while (name = properties->getNextProperty())
    {
        if (strcmp(name, "type") == 0)
        {
            std::string typeStr = properties->getString();
            if (typeStr == "BOX")
                type = SHAPE_BOX;
            else if (typeStr == "SPHERE")
                type = SHAPE_SPHERE;
            else if (typeStr == "MESH")
                type = SHAPE_MESH;
            else if (typeStr == "HEIGHTFIELD")
                type = SHAPE_HEIGHTFIELD;
            else if (typeStr == "CAPSULE")
                type = SHAPE_CAPSULE;
            else
            {
                WARN_VARG("Could not create rigid body; unsupported value for rigid body type: '%s'.", typeStr.c_str());
                return NULL;
            }
        }
        else if (strcmp(name, "mass") == 0)
        {
            mass = properties->getFloat();
        }
        else if (strcmp(name, "friction") == 0)
        {
            friction = properties->getFloat();
        }
        else if (strcmp(name, "restitution") == 0)
        {
            restitution = properties->getFloat();
        }
        else if (strcmp(name, "linearDamping") == 0)
        {
            linearDamping = properties->getFloat();
        }
        else if (strcmp(name, "angularDamping") == 0)
        {
            angularDamping = properties->getFloat();
        }
        else if (strcmp(name, "kinematic") == 0)
        {
            kinematic = properties->getBool();
        }
        else if (strcmp(name, "gravity") == 0)
        {
            gravity = new Vector3();
            properties->getVector3(NULL, gravity);
        }
        else if (strcmp(name, "anisotropicFriction") == 0)
        {
            anisotropicFriction = new Vector3();
            properties->getVector3(NULL, anisotropicFriction);
        }
        else if (strcmp(name, "image") == 0)
        {
            imagePath = properties->getString();
        }
        else if (strcmp(name, "radius") == 0)
        {
            radius = properties->getFloat();
        }
        else if (strcmp(name, "height") == 0)
        {
            height = properties->getFloat();
        }
    }

    // If the rigid body type is equal to mesh, check that the node's mesh's primitive type is supported.
    if (type == SHAPE_MESH)
    {
        Mesh* mesh = node->getModel()->getMesh();

        switch (mesh->getPrimitiveType())
        {
        case Mesh::TRIANGLES:
            break;
        case Mesh::LINES:
        case Mesh::LINE_STRIP:
        case Mesh::POINTS:
        case Mesh::TRIANGLE_STRIP:
            WARN("Mesh rigid bodies are currently only supported on meshes with primitive type equal to TRIANGLES.");

            SAFE_DELETE(gravity);
            SAFE_DELETE(anisotropicFriction);
            return NULL;
        }
    }

    // Create the rigid body.
    PhysicsRigidBody* body = NULL;
    switch (type)
    {
        case SHAPE_HEIGHTFIELD:
            if (imagePath == NULL)
            {
                WARN("Heightfield rigid body requires an image path.");
            }
            else
            {
                // Load the image data from the given file path.
                Image* image = Image::create(imagePath);
                if (!image)
                    return NULL;

                // Ensure that the image's pixel format is supported.
                switch (image->getFormat())
                {
                    case Image::RGB:
                    case Image::RGBA:
                        break;
                    default:
                        WARN_VARG("Heightmap: pixel format is not supported: %d", image->getFormat());
                        return NULL;
                }

                body = new PhysicsRigidBody(node, image, mass, friction, restitution, linearDamping, angularDamping);
                SAFE_RELEASE(image);
            }
            break;
        case SHAPE_CAPSULE:
            if (radius == -1.0f || height == -1.0f)
            {
                WARN("Both 'radius' and 'height' must be specified for a capsule rigid body.");
            }
            else
            {
                body = new PhysicsRigidBody(node, radius, height, mass, friction, restitution, linearDamping, angularDamping);
            }
            break;
        default:
            body = new PhysicsRigidBody(node, type, mass, friction, restitution, linearDamping, angularDamping);
            break;
    }

    // Set any initially defined properties.
    if (kinematic)
        body->setKinematic(kinematic);
    if (gravity)
        body->setGravity(*gravity);
    if (anisotropicFriction)
        body->setAnisotropicFriction(*anisotropicFriction);

    // Clean up any loaded properties that are on the heap.
    SAFE_DELETE(gravity);
    SAFE_DELETE(anisotropicFriction);

    return body;
}

float PhysicsRigidBody::getHeight(float x, float y) const
{
    // This function is only supported for heightfield rigid bodies.
    if (_shape->getShapeType() != TERRAIN_SHAPE_PROXYTYPE)
    {
        WARN("Attempting to get the height of a non-heightfield rigid body.");
        return 0.0f;
    }

    // Calculate the correct x, y position relative to the heightfield data.
    if (_inverseIsDirty)
    {
        if (_inverse == NULL)
            _inverse = new Matrix();

        _node->getWorldMatrix().invert(_inverse);
        _inverseIsDirty = false;
    }

    Vector3 v = (*_inverse) * Vector3(x, 0.0f, y);
    x = (v.x + (0.5f * (_width - 1))) * _width / (_width - 1);
    y = (v.z + (0.5f * (_height - 1))) * _height / (_height - 1);

    // Check that the x, y position is within the bounds.
    if (x < 0.0f || x > _width || y < 0.0f || y > _height)
    {
        WARN_VARG("Attempting to get height at point '%f, %f', which is outside the range of the heightfield with width %d and height %d.", x, y, _width, _height);
        return 0.0f;
    }

    return calculateHeight(_heightfieldData, _width, _height, x, y);
}

btRigidBody* PhysicsRigidBody::createRigidBodyInternal(btCollisionShape* shape, float mass, Node* node,
                                                       float friction, float restitution, float linearDamping, float angularDamping, 
                                                       const Vector3* centerOfMassOffset)
{
    // If the mass is non-zero, then the object is dynamic so we calculate the local 
    // inertia. However, if the collision shape is a triangle mesh, we don't calculate 
    // inertia since Bullet doesn't currently support this.
    btVector3 localInertia(0.0, 0.0, 0.0);
    if (mass != 0.0 && shape->getShapeType() != TRIANGLE_MESH_SHAPE_PROXYTYPE)
        shape->calculateLocalInertia(mass, localInertia);

    // Create the Bullet physics rigid body object.
    PhysicsMotionState* motionState = new PhysicsMotionState(node, centerOfMassOffset);
    btRigidBody::btRigidBodyConstructionInfo rbInfo(mass, motionState, shape, localInertia);
    rbInfo.m_friction = friction;
    rbInfo.m_restitution = restitution;
    rbInfo.m_linearDamping = linearDamping;
    rbInfo.m_angularDamping = angularDamping;
    btRigidBody* body = bullet_new<btRigidBody>(rbInfo);

    return body;
}

void PhysicsRigidBody::addConstraint(PhysicsConstraint* constraint)
{
    _constraints.push_back(constraint);
}

void PhysicsRigidBody::removeConstraint(PhysicsConstraint* constraint)
{
    for (unsigned int i = 0; i < _constraints.size(); i++)
    {
        if (_constraints[i] == constraint)
        {
            _constraints.erase(_constraints.begin() + i);
            break;
        }
    }
}

bool PhysicsRigidBody::supportsConstraints()
{
    return _shape->getShapeType() != TRIANGLE_MESH_SHAPE_PROXYTYPE && _shape->getShapeType() != TERRAIN_SHAPE_PROXYTYPE;
}

void PhysicsRigidBody::transformChanged(Transform* transform, long cookie)
{
    _inverseIsDirty = true;
}

float calculateHeight(float* data, unsigned int width, unsigned int height, float x, float y)
{
    unsigned int x1 = x;
    unsigned int y1 = y;
    unsigned int x2 = x1 + 1;
    unsigned int y2 = y1 + 1;
    float tmp;
    float xFactor = modf(x, &tmp);
    float yFactor = modf(y, &tmp);
    float xFactorI = 1.0f - xFactor;
    float yFactorI = 1.0f - yFactor;

    if (x2 >= width && y2 >= height)
    {
        return data[x1 + y1 * width];
    }
    else if (x2 >= width)
    {
        return data[x1 + y1 * width] * yFactorI + data[x1 + y2 * width] * yFactor;
    }
    else if (y2 >= height)
    {
        return data[x1 + y1 * width] * xFactorI + data[x2 + y1 * width] * xFactor;
    }
    else
    {
        return data[x1 + y1 * width] * xFactorI * yFactorI + data[x1 + y2 * width] * xFactorI * yFactor + 
            data[x2 + y2 * width] * xFactor * yFactor + data[x2 + y1 * width] * xFactor * yFactorI;
    }
}

}
